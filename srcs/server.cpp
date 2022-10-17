/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   server.cpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: nsartral <nsartral@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2022/09/28 13:45:50 by bdetune           #+#    #+#             */
/*   Updated: 2022/10/17 13:30:12 by bdetune          ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "server.hpp"

Server::Server(void): _filesMoving(false), _epoll_fd(-1), _watchedEvents(NULL)
{
	try
	{
		this->_watchedEvents = new struct epoll_event[WATCHEDEVENTS];
		this->_rdBuffer = new char[READCHUNKSIZE + 1];
		this->_rdBufferCpy.reserve((READCHUNKSIZE + 1));
	}
	catch (std::exception const & e)
	{
		std::cerr << "Error while trying to create server: " << e.what() << std::endl;
		throw std::bad_alloc();
	}
	return ;
}

Server::~Server(void)
{
	delete [] this->_rdBuffer;
	if (this->_watchedEvents)
		delete [] this->_watchedEvents;
	if (this->_epoll_fd != -1)
		close(this->_epoll_fd);
	for (std::map<int, Client>::iterator st = this->_client_sockets.begin(); st!= this->_client_sockets.end(); st++)
	{
		shutdown((*st).first, SHUT_RDWR);
		close((*st).first);
	}
	for (std::map<int, int>::iterator st = this->_listen_sockets.begin(); st!= this->_listen_sockets.end(); st++)
	{
		close((*st).first);
	}
	this->_client_sockets.clear();
	this->_listen_sockets.clear();
}

struct sockaddr_in Server::getAddr(void)
{
	return (this->_address);
}

void Server::initing(std::vector<ServerScope> & virtual_servers)
{
	int	_server_fd;
	int	enable = 1;

	for (std::vector<ServerScope>::iterator first = virtual_servers.begin(); first != virtual_servers.end(); first++)
	{
		this->_virtual_servers[atoi(first->getPort().data())].push_back(*first);
	}
	for (std::map<int, std::vector<ServerScope> >::iterator first = this->_virtual_servers.begin(); first != this->_virtual_servers.end(); first++)
	{	
		if ((_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) // creating the socket
			throw SocketCreationException();
		_address.sin_family = AF_INET; // socket configuration
		_address.sin_addr.s_addr = INADDR_ANY;
		_address.sin_port = htons( (*first).first );
		memset(_address.sin_zero, '\0', sizeof _address.sin_zero); // applying configurations on the created socket
		if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)))
			throw BindException();
		if (bind(_server_fd, (struct sockaddr *)&_address, sizeof(_address)) < 0)
			throw BindException();
		if (listen(_server_fd, 1000) < 0) // opening the socket to the port
			throw ListenException();
		this->_listen_sockets.insert(std::pair<int, int>(_server_fd, (*first).first));
	}
	if ((this->_epoll_fd = epoll_create(1)) == -1)
		throw EpollCreateException();
}

bool	Server::epollSockets(void)
{
	std::memset(&(this->_tmpEv), '\0', sizeof(struct epoll_event));
	for (std::map<int, int>::iterator st = this->_listen_sockets.begin(); st!= this->_listen_sockets.end(); st++)
	{
		this->_tmpEv.events = EPOLLIN;
		this->_tmpEv.data.fd = (*st).first;
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, (*st).first, &(this->_tmpEv)) == -1)
			return (false) ;
	}
	return (true);
}

void	Server::acceptIncomingConnection(struct epoll_event & event)
{
	Client				tmp(this->_listen_sockets[event.data.fd]);
    int					addrlen	=sizeof(_address);

	std::memset(&(this->_tmpEv), '\0', sizeof(struct epoll_event));
	this->_tmpEv.events = EPOLLIN;
	this->_tmpEv.data.fd = accept(event.data.fd, (struct sockaddr *)&_address, (socklen_t*)&addrlen);
	if (this->_tmpEv.data.fd < 0)	// If client socket cannot be created
		return ;
	tmp.setIpAddress(_address.sin_addr.s_addr);
	tmp.setSocketFD(this->_tmpEv.data.fd);
	tmp.setEvent(this->_tmpEv);
	if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, this->_tmpEv.data.fd, &(this->_tmpEv)) == -1)
	{
		close(this->_tmpEv.data.fd);
		return ;
	}
	try
	{
		this->_client_sockets.insert(std::pair<int, Client>(this->_tmpEv.data.fd, tmp));
	}
	catch (std::exception const & e)
	{
		std::cerr << "Could not insert new client in map: " << e.what() << std::endl;
		epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, this->_tmpEv.data.fd, &(this->_tmpEv));
		close(this->_tmpEv.data.fd);
	}
	/*
	unsigned long address = ntohl(_address.sin_addr.s_addr);
	std::cout << "Address: ";
	std::cout << (address >> 24) << ".";
	std::cout << ((address >> 16) & 255) << ".";
	std::cout << ((address >> 8) & 255) << ".";
	std::cout << (address & 255) << std::endl;
	*/
}

void	Server::readToWrite(struct epoll_event & event, Client & currentClient, Request & currentRequest, Response & currentResponse)
{
	if (!currentResponse.serverSet())
	{
		currentResponse = Response(currentRequest, this->_virtual_servers[currentClient.getPortNumber()]);
		if (!currentResponse.precheck(currentRequest))
		{
			event.events = EPOLLOUT;
			if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event) == -1)
				this->closeClientSocket(event);
			return ;
		}
	}
	currentResponse.makeResponse(currentRequest);
	if (currentRequest.getType() == std::string("PUT") && !currentResponse.getBodySize())
	{
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event) == -1)
		{
			this->closeClientSocket(event);
			return ;
		}
		this->_filesMoving = true;
		this->_filesMovingClients.push_back(&currentClient);
		return ;
	}
	else if (currentResponse.isCgi())
	{
		std::memset(&(this->_tmpEv), '\0', sizeof(struct epoll_event));
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event) == -1)
		{
			this->closeClientSocket(event);
			return ;
		}
		this->_cgi_pipes[currentResponse.getCgiFd()] = event.data.fd;
		this->_tmpEv.events = EPOLLIN;
		this->_tmpEv.data.fd = currentResponse.getCgiFd();
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, this->_tmpEv.data.fd, &(this->_tmpEv)) == -1)
		{
			this->_cgi_pipes.erase(currentResponse.getCgiFd());
			this->closeClientSocket(event);
			return ;
		}
	}
	else
	{
		event.events = EPOLLOUT;
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event) == -1)
			this->closeClientSocket(event);
	}
}

void	Server::readRequest(struct epoll_event & event)
{
	int			result;
	ssize_t		recvret = 0;
	Client &	currentClient = this->_client_sockets[event.data.fd];
	Request &	currentRequest = this->_client_sockets[event.data.fd].getRequest();
	Response &	currentResponse = this->_client_sockets[event.data.fd].getResponse();

	recvret = recv(event.data.fd, static_cast<void *>(this->_rdBuffer), READCHUNKSIZE, MSG_DONTWAIT);
	if (recvret == -1 || recvret == 0)
		this->closeClientSocket(event);
	else
	{
		try
		{
			this->_rdBufferCpy.assign(this->_rdBuffer, (this->_rdBuffer + recvret));
			result = currentClient.addToRequest(this->_rdBufferCpy);
		}
		catch (std::exception const & e)
		{
			this->closeClientSocket(event);
			return ;
		}
		if (result == 201)
		{
			if (currentResponse.serverSet())
				return ;
			currentResponse = Response(currentRequest, this->_virtual_servers[currentClient.getPortNumber()]);
			if (!currentResponse.precheck(currentRequest))
			{
				event.events = EPOLLOUT;
				if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event) == -1)
					this->closeClientSocket(event);
			}
		}
		else if (result == 200)
			this->readToWrite(event, currentClient, currentRequest, currentResponse);
		else if (result)
		{
			if (result == 1)
			{
				this->closeClientSocket(event);
				return ;
			}
			currentResponse = Response(currentRequest, this->_virtual_servers[currentClient.getPortNumber()], result);
			event.events = EPOLLOUT;
			if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event) == -1)
				this->closeClientSocket(event);
		}
	}
}

bool	Server::sendHeader(struct epoll_event & event, Client & currentClient)
{
	ssize_t	sendret = 0;

	sendret = send(event.data.fd, currentClient.getResponse().getHeader().data(), currentClient.getResponse().getHeaderSize(), MSG_NOSIGNAL | MSG_DONTWAIT);
	if (sendret == -1)
	{
		if (currentClient.getResponse().isCgi())
			this->_cgi_pipes.erase(currentClient.getResponse().getCgiFd());
		this->closeClientSocket(event);
		return (false) ;
	}
	return (currentClient.getResponse().headerBytesSent(sendret));
}

void	Server::sendBody(struct epoll_event & event, Client & currentClient)
{
	Response &	currentResponse = currentClient.getResponse();
	ssize_t	sendret = 0;

	sendret = send(event.data.fd, currentResponse.getBody().data(), currentResponse.getBodySize(), MSG_NOSIGNAL | MSG_DONTWAIT);
	if (sendret == -1)
	{
		if (currentResponse.isCgi())
			this->_cgi_pipes.erase(currentResponse.getCgiFd());
		this->closeClientSocket(event);
		return ;
	}
	if (currentResponse.bodyBytesSent(sendret))
	{
		if (currentResponse.isCgi())
		{
			this->_cgi_pipes.erase(currentResponse.getCgiFd());
			currentResponse.closeCgiFd();
		}
		if (currentResponse.getClose())
			this->closeClientSocket(event);
		else
		{
			currentClient.resetRequest();
			currentClient.resetResponse();
			event.events = EPOLLIN;
			if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event))
				this->closeClientSocket(event);
		}
	}
	else if (currentResponse.isCgi() && !currentResponse.getBodySize())
	{
		std::memset(&(this->_tmpEv), '\0', sizeof(struct epoll_event));
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event) == -1)
		{
			this->_cgi_pipes.erase(currentResponse.getCgiFd());
			this->closeClientSocket(event);
			return ;
		}
		this->_tmpEv.events = EPOLLIN;
		this->_tmpEv.data.fd = currentResponse.getCgiFd();
		this->_cgi_pipes[currentResponse.getCgiFd()] = event.data.fd;
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, this->_tmpEv.data.fd, &(this->_tmpEv)) == -1)
		{
			this->_cgi_pipes.erase(currentResponse.getCgiFd());
			this->closeClientSocket(event);
			return ;
		}
	}
}

void	Server::closeClientSocket(struct epoll_event & event)
{
	epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event);
	close(event.data.fd);
	this->_client_sockets.erase(event.data.fd);
}

void	Server::sendResponse(struct epoll_event & event)
{
	Client &	currentClient = this->_client_sockets[event.data.fd];

	if (!currentClient.getResponse().headerIsSent())
	{
		if (this->sendHeader(event, currentClient))
		{
			if (!currentClient.getResponse().getBodySize())
			{
				if (currentClient.getResponse().isCgi())
				{
					this->_cgi_pipes.erase(currentClient.getResponse().getCgiFd());
					currentClient.getResponse().closeCgiFd();
				}
				if (currentClient.getResponse().getClose())
					this->closeClientSocket(event);
				else
				{
					currentClient.resetRequest();
					currentClient.resetResponse();
					event.events = EPOLLIN;
					if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_MOD, event.data.fd, &event))
						this->closeClientSocket(event);
				}
			}
		}
	}
	else
		this->sendBody(event, currentClient);
}

void	Server::readPipe(struct epoll_event & event)
{
	bool			redirection = false;
	ServerScope*	targetServer;
	Client	& currentClient = this->_client_sockets[this->_cgi_pipes[event.data.fd]];
	Response & currentResponse = currentClient.getResponse();

	std::cerr << event.data.fd << std::endl; 
	redirection = currentResponse.cgiResponse(event.data.fd);
	if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_DEL, event.data.fd, &event) == -1)
	{
		this->_cgi_pipes.erase(event.data.fd);
		this->closeClientSocket(currentClient.getEvent());
		return ;
	}
	if (redirection)
	{
		targetServer = currentResponse.getServerScope();
		currentClient.resetResponse();
		currentResponse.setServerScope(targetServer);
		this->_cgi_pipes.erase(event.data.fd);
		if (!currentResponse.precheck(currentClient.getRequest()))
		{
			std::cerr << "Well that did not work" << std::endl;
			currentClient.getEvent().events = EPOLLOUT;
			currentClient.getEvent().data.fd = currentClient.getSocketFD();
			if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, currentClient.getSocketFD(), &(currentClient.getEvent())) == -1)
			{
				close(currentClient.getSocketFD());
				this->_client_sockets.erase(currentClient.getSocketFD());
			}
			return ;
		}
		currentClient.getEvent().events = EPOLLIN;
		currentClient.getEvent().data.fd = currentClient.getSocketFD();
		if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, currentClient.getSocketFD(), &(currentClient.getEvent())) == -1)
		{
			close(currentClient.getSocketFD());
			this->_client_sockets.erase(currentClient.getSocketFD());
			return ;
		}
		this->readToWrite(currentClient.getEvent(), currentClient, currentClient.getRequest(), currentClient.getResponse());
		return ;
	}
	currentClient.getEvent().events = EPOLLOUT;
	currentClient.getEvent().data.fd = currentClient.getSocketFD();
	if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, currentClient.getSocketFD(), &(currentClient.getEvent())) == -1)
	{
		this->_cgi_pipes.erase(event.data.fd);
		close(currentClient.getSocketFD());
		this->_client_sockets.erase(currentClient.getSocketFD());
		return ;
	}
}

void Server::execute(void)
{
	int ready;
	int	timeout;


	if (!this->epollSockets())
	{
		delete [] this->_watchedEvents;
		throw EpollCreateException();
	}
	while (true)
	{
		timeout = this->_filesMoving ? 0 : 1000;
		ready = epoll_wait(this->_epoll_fd, this->_watchedEvents, WATCHEDEVENTS, timeout);
		if (ready == -1 || g_code)
			return ;
		for (int i = 0; i < ready; i++)
		{
			if (this->_listen_sockets.find(this->_watchedEvents[i].data.fd) != this->_listen_sockets.end())
				this->acceptIncomingConnection(this->_watchedEvents[i]);
			else if (this->_cgi_pipes.find(this->_watchedEvents[i].data.fd) != this->_cgi_pipes.end())
				this->readPipe(this->_watchedEvents[i]);
			else if (this->_client_sockets.find(this->_watchedEvents[i].data.fd) != this->_client_sockets.end())
			{
				switch (this->_watchedEvents[i].events)
				{
					case EPOLLOUT:
						this->sendResponse(this->_watchedEvents[i]);
						break;
					case EPOLLIN:
						this->readRequest(this->_watchedEvents[i]);
						break;
					default:
						if (this->_client_sockets[this->_watchedEvents[i].data.fd].getResponse().isCgi())
							this->_cgi_pipes.erase(this->_client_sockets[this->_watchedEvents[i].data.fd].getResponse().getCgiFd());
						this->closeClientSocket(this->_watchedEvents[i]);
						break;
				}
			}
        }
		if (this->_filesMoving)
			this->moveFiles();
		this->closeTimedoutConnections();
		// std::cout << "The number of clients is: " << this->_client_sockets.size() << std::endl;
    }
}

void	Server::moveFiles(void)
{
	bool	finished;

	for (std::vector<Client *>::reverse_iterator st = this->_filesMovingClients.rbegin(); st != this->_filesMovingClients.rend(); st++)
	{
		finished = (*st)->getRequest().moveBody((*st)->getResponse().getTargetFile());
		if (finished)
		{
			(*st)->getEvent().events = EPOLLIN;
			(*st)->getEvent().data.fd = (*st)->getSocketFD();
			if (epoll_ctl(this->_epoll_fd, EPOLL_CTL_ADD, (*st)->getSocketFD(), &((*st)->getEvent())) == -1)
				this->closeClientSocket((*st)->getEvent());
			std::cerr << "Finished moving file" << std::endl;
			this->_filesMovingClients.erase(st.base());	
		}
	}
	if (this->_filesMovingClients.size() == 0)
		this->_filesMoving = false;
}

void	Server::closeTimedoutConnections(void)
{
	std::time_t	current = std::time(NULL);
	std::map<int, Client>::iterator tmp;

	for (std::map<int, Client>::iterator st = this->_client_sockets.begin(); st != this->_client_sockets.end(); st = tmp)
	{
		tmp = st;
		tmp++;
		if (std::difftime(current, (*st).second.getLastConnection()) > (*st).second.getKeepAlive())
			this->closeClientSocket((*st).second.getEvent());
	}
}
