//
// tcpproxy_server.cpp
// ~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2007 Arash Partow (http://www.partow.net)
// URL: http://www.partow.net/programming/tcpproxy/index.html
//
// Distributed under the Boost Software License, Version 1.0.
//
//
// Description
// ~~~~~~~~~~~
// The  objective of  the TCP  proxy server  is to  act  as  an
// intermediary  in order  to 'forward'  TCP based  connections
// from external clients onto a singular remote server.
//
// The communication flow in  the direction from the  client to
// the proxy to the server is called the upstream flow, and the
// communication flow in the  direction from the server  to the
// proxy  to  the  client   is  called  the  downstream   flow.
// Furthermore  the   up  and   down  stream   connections  are
// consolidated into a single concept known as a bridge.
//
// In the event  either the downstream  or upstream end  points
// disconnect, the proxy server will proceed to disconnect  the
// other  end  point  and  eventually  destroy  the  associated
// bridge.
//
// The following is a flow and structural diagram depicting the
// various elements  (proxy, server  and client)  and how  they
// connect and interact with each other.

//
//                                    ---> upstream --->           +---------------+
//                                                     +---->------>               |
//                               +-----------+         |           | Remote Server |
//                     +--------->          [x]--->----+  +---<---[x]              |
//                     |         | TCP Proxy |            |        +---------------+
// +-----------+       |  +--<--[x] Server   <-----<------+
// |          [x]--->--+  |      +-----------+
// |  Client   |          |
// |           <-----<----+
// +-----------+
//                <--- downstream <---
//
//


#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <string>

#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/thread/mutex.hpp>
#include <algorithm>

namespace tcp_proxy
{
	namespace ip = boost::asio::ip;

	class sym_crypto
	{
	public:
		sym_crypto() {}
		virtual ~sym_crypto() {}
		virtual void crypt(void *data, size_t size) {}
	};

	class xor_key_crypto : public sym_crypto
	{
		std::string key;
		int idx;
	public:
		xor_key_crypto(std::string key) : key(key), idx(0) {}
		void crypt(void *data, size_t size) override
		{
			uint8_t *p = (uint8_t*)data;
			while (size-- > 0)
			{
				*p++ ^= key[idx];
				idx = (idx + 1) % key.size();
			}
		}
	};

	class rc4_crypto : public sym_crypto
	{
	public:
		rc4_crypto(std::string key)
		{
			rc4_init((const unsigned char*)key.c_str(), key.size());
		}
		void crypt(void *data, size_t size) override
		{
			uint8_t *p = (uint8_t*)data;
			while (size-- > 0)
				*p++ ^= rc4_output();
		}
	private:
		unsigned char S[256];
		unsigned int i, j;

		inline void swap(unsigned char *s, unsigned int i, unsigned int j) {
			unsigned char temp = s[i];
			s[i] = s[j];
			s[j] = temp;
		}

		/* KSA */
		inline void rc4_init(const unsigned char *key, unsigned int key_length) {
			for (i = 0; i < 256; i++)
				S[i] = i;

			for (i = j = 0; i < 256; i++) {
				j = (j + key[i % key_length] + S[i]) & 255;
				swap(S, i, j);
			}

			i = j = 0;
		}

		/* PRGA */
		inline unsigned char rc4_output() {
			i = (i + 1) & 255;
			j = (j + S[i]) & 255;

			swap(S, i, j);

			return S[(S[i] + S[j]) & 255];
		}
	};

	class bridge : public boost::enable_shared_from_this<bridge>
	{
	public:

		typedef ip::tcp::socket socket_type;
		typedef boost::shared_ptr<bridge> ptr_type;

		bridge(boost::asio::io_service& ios, const std::string& key)
			: resolver_(ios),
			downstream_socket_(ios),
			upstream_socket_(ios),
			key_(key),
			downstream_crypto_(nullptr),
			upstream_crypto_(nullptr),
			downstream_read_close_(false),
			upstream_read_close_(false),
			downstream_write_close_(false),
			upstream_write_close_(false)
		{
			if (!key_.empty())
			{
				downstream_crypto_ = new rc4_crypto(key_);
				upstream_crypto_ = new rc4_crypto(key_);
			}
		}

		socket_type& downstream_socket()
		{
			// Client socket
			return downstream_socket_;
		}

		socket_type& upstream_socket()
		{
			// Remote server socket
			return upstream_socket_;
		}

		void start(const std::string& upstream_host, unsigned short upstream_port)
		{
			// Attempt connection to remote server (upstream side)
			char port[32];
			sprintf(port, "%d", upstream_port);
			boost::asio::ip::tcp::resolver::query query(upstream_host, port);

			boost::asio::ip::tcp::resolver::iterator iterator = resolver_.resolve(query);
			boost::asio::async_connect(upstream_socket_, 
				iterator,
				boost::bind(&bridge::handle_upstream_connect,
					shared_from_this(),
					boost::asio::placeholders::error));
		}

		void handle_upstream_connect(const boost::system::error_code& error)
		{
			if (!error)
			{
				// Setup async read from remote server (upstream)
				upstream_socket_.async_read_some(
					boost::asio::buffer(upstream_data_, max_data_length),
					boost::bind(&bridge::handle_upstream_read,
						shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred));

				// Setup async read from client (downstream)
				downstream_socket_.async_read_some(
					boost::asio::buffer(downstream_data_, max_data_length),
					boost::bind(&bridge::handle_downstream_read,
						shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred));
			}
			else
				close();
		}

	private:

		/*
		   Section A: Remote Server --> Proxy --> Client
		   Process data recieved from remote sever then send to client.
		*/

		// Read from remote server complete, now send data to client
		void handle_upstream_read(const boost::system::error_code& error,
			const size_t& bytes_transferred)
		{
			if (!error)
			{
				if(upstream_crypto_)
					upstream_crypto_->crypt(upstream_data_, bytes_transferred);
				async_write(downstream_socket_,
					boost::asio::buffer(upstream_data_, bytes_transferred),
					boost::bind(&bridge::handle_downstream_write,
						shared_from_this(),
						boost::asio::placeholders::error));
			}
			else
			{
				upstream_read_close_ = true;
				prepare_close();
			}
		}

		// Write to client complete, Async read from remote server
		void handle_downstream_write(const boost::system::error_code& error)
		{
			if (!error)
			{
				upstream_socket_.async_read_some(
					boost::asio::buffer(upstream_data_, max_data_length),
					boost::bind(&bridge::handle_upstream_read,
						shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred));
			}
			else
			{
				downstream_write_close_ = true;
				prepare_close();
			}
		}
		// *** End Of Section A ***


		/*
		   Section B: Client --> Proxy --> Remove Server
		   Process data recieved from client then write to remove server.
		*/

		// Read from client complete, now send data to remote server
		void handle_downstream_read(const boost::system::error_code& error,
			const size_t& bytes_transferred)
		{
			if (!error)
			{
				if(downstream_crypto_)
					downstream_crypto_->crypt(downstream_data_, bytes_transferred);
				async_write(upstream_socket_,
					boost::asio::buffer(downstream_data_, bytes_transferred),
					boost::bind(&bridge::handle_upstream_write,
						shared_from_this(),
						boost::asio::placeholders::error));
			}
			else
			{
				downstream_read_close_ = true;
				prepare_close();
			}
		}

		// Write to remote server complete, Async read from client
		void handle_upstream_write(const boost::system::error_code& error)
		{
			if (!error)
			{
				downstream_socket_.async_read_some(
					boost::asio::buffer(downstream_data_, max_data_length),
					boost::bind(&bridge::handle_downstream_read,
						shared_from_this(),
						boost::asio::placeholders::error,
						boost::asio::placeholders::bytes_transferred));
			}
			else
			{
				upstream_write_close_ = true;
				prepare_close();
			}
		}
		// *** End Of Section B ***

		void prepare_close()
		{
			if (downstream_read_close_ && upstream_read_close_ || downstream_write_close_ && upstream_write_close_ 
				|| upstream_read_close_ && upstream_write_close_ || downstream_read_close_ && downstream_write_close_)
				close();
		}

		void close()
		{
			boost::mutex::scoped_lock lock(mutex_);

			if (downstream_socket_.is_open())
			{
				downstream_socket_.close();
			}

			if (upstream_socket_.is_open())
			{
				upstream_socket_.close();
			}
			delete downstream_crypto_;
			downstream_crypto_ = nullptr;
			delete upstream_crypto_;
			upstream_crypto_ = nullptr;
		}

		socket_type downstream_socket_;
		socket_type upstream_socket_;
		bool downstream_read_close_;
		bool upstream_read_close_;
		bool downstream_write_close_;
		bool upstream_write_close_;

		ip::tcp::resolver resolver_;

		enum { max_data_length = 8192 }; //8KB
		unsigned char downstream_data_[max_data_length];
		unsigned char upstream_data_[max_data_length];

		boost::mutex mutex_;

		std::string key_;
		sym_crypto * downstream_crypto_;
		sym_crypto * upstream_crypto_;

	public:

		class acceptor
		{
		public:

			acceptor(boost::asio::io_service& io_service,
				const std::string& local_host, unsigned short local_port,
				const std::string& upstream_host, unsigned short upstream_port, const std::string& key)
				: io_service_(io_service),
				localhost_address(boost::asio::ip::address_v4::from_string(local_host)),
				acceptor_(io_service_, ip::tcp::endpoint(localhost_address, local_port)),
				upstream_port_(upstream_port),
				upstream_host_(upstream_host),
				key_(key)
			{}

			bool accept_connections()
			{
				try
				{
					session_ = boost::shared_ptr<bridge>(new bridge(io_service_, key_));

					acceptor_.async_accept(session_->downstream_socket(),
						boost::bind(&acceptor::handle_accept,
							this,
							boost::asio::placeholders::error));
				}
				catch (std::exception& e)
				{
					std::cerr << "acceptor exception: " << e.what() << std::endl;
					return false;
				}

				return true;
			}

		private:

			void handle_accept(const boost::system::error_code& error)
			{
				if (!error)
				{
					std::cerr << "Accepted from " << session_->downstream_socket().remote_endpoint().address().to_string() << " " <<
						session_->downstream_socket().remote_endpoint().port() << std::endl;
					session_->start(upstream_host_, upstream_port_);

					if (!accept_connections())
					{
						std::cerr << "Failure during call to accept." << std::endl;
					}
				}
				else
				{
					std::cerr << "Error: " << error.message() << std::endl;
				}
			}

			boost::asio::io_service& io_service_;
			ip::address_v4 localhost_address;
			ip::tcp::acceptor acceptor_;
			ptr_type session_;
			unsigned short upstream_port_;
			std::string upstream_host_;
			std::string key_;
		};

	};
}

int start_proxy(const std::string& local_host, const unsigned short local_port, const std::string& forward_host, const unsigned short forward_port, const std::string& key)
{
	boost::asio::io_service ios;

	try
	{
		tcp_proxy::bridge::acceptor acceptor(ios,
			local_host, local_port,
			forward_host, forward_port, key);

		acceptor.accept_connections();

		ios.run();
	}
	catch (std::exception& e)
	{
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}

#ifdef FOR_MAIN
int main(int argc, char* argv[])
{
	if (argc < 5)
	{
		std::cerr << "usage: tcpproxy_server <local host ip> <local port> <forward host ip> <forward port> <key>" << std::endl;
		return 1;
	}

	const unsigned short local_port = static_cast<unsigned short>(::atoi(argv[2]));
	const unsigned short forward_port = static_cast<unsigned short>(::atoi(argv[4]));
	const std::string local_host = argv[1];
	const std::string forward_host = argv[3];
	std::string key;
	if (argc > 5)
		key = argv[5];

	return start_proxy(local_host, local_port, forward_host, forward_port, key);
}
#endif
/*
 * [Note] On posix systems the tcp proxy server build command is as follows:
 * c++ -pedantic -ansi -Wall -Werror -O3 -o tcpproxy_server tcpproxy_server.cpp -L/usr/lib -lstdc++ -lpthread -lboost_thread -lboost_system
 */
