#include <unistd.h>
#include <signal.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <errno.h>
#include <functional>
#include <iostream>
#include <map>
#include <vector>
#include <queue>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

#ifndef INVALID_SOCKET
  #define INVALID_SOCKET -1
#endif

static int last_errno()
{
#if defined(_WIN32)
    return GetLastError();
#elif defined(__linux__)
    return errno;
#endif
}


using SOCKETTYPE = int;
using TimeProcessorFn = std::function<void(int, int, int)>;
struct STimer
{
    int64_t id = 0;
    uint64_t trigger_time  = 0 ; // million sec
    int delay = 0;
    int interval  = 0;

    TimeProcessorFn processer;

    bool operator < (const STimer& a) const
    {
        return this->trigger_time > a.trigger_time;
    }
};

class STimerQueue : public std::priority_queue<STimer>
{
public:
    bool remove(int id) {
        auto it = std::find_if(this->c.begin(), this->c.end(), [id](auto& item) {return item.id == id; });
        if (it != this->c.end()) {
            this->c.erase(it);
            std::make_heap(this->c.begin(), this->c.end(), this->comp);
            return true;
        }
        else {
            return false;
        }
    }

    bool exist(int id)
    {
        auto it = std::find_if(this->c.begin(), this->c.end(), [id](auto& item) {return item.id == id; });
        return it != this->c.end();
    }
};


enum class SockState
{
    SS_CLOSED = 0,
    SS_CLOSING,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_LISTENING
};

enum class SEventType
{
    ET_CLOSE = 0,
    ET_CONNECT,
    ET_ERROR,
    ET_WRITE_READY,
    ET_DATA,
    ET_ACCETP,
    ET_LISTEN,
};

class SockHandler;
struct SEventArgs
{
    SEventArgs(SEventType t, int ec) : etype(t), code(ec)
    {
        buff = nullptr;
        size = 0;
    }

    SEventType etype;
    int code;
    char *buff;
    size_t size;
    SockHandler*accepted_handler = nullptr;
};

class SockHandler
{
public:
    using ListenFnType = std::function<void(SockHandler *, SEventArgs)>;

public:
    uint64_t mId = 0;
    SOCKETTYPE mSocket = 0;
    SockState mState = SockState::SS_CLOSED;
    //std::vector<char> mInBuf;
    std::vector<char> mOutBuf;

public:
    std::map<SEventType, ListenFnType> mListener;

public:
    SockHandler()
    {
        mSocket = socket(AF_INET, SOCK_STREAM, 0);
        this->SetNoBlock();
    }

    SockHandler(SOCKETTYPE socket) : mSocket(socket)
    {

    }
    SOCKETTYPE Fd()
    {
        return mSocket;
    }

    void SetNoDelay(int opt)
    {
        opt = !!opt;
        setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
    }

    void SetNoBlock()
    {
        int opt = 1;
#ifdef _WIN32
        u_long mode = opt;
        ioctlsocket(stream->sockfd, FIONBIO, &mode);
#else
        int flags = fcntl(mSocket, F_GETFL);
        fcntl(mSocket, F_SETFL,
            opt ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
    }
    void SetREUSEADDR()
    {
        int optval = 1;
        setsockopt(mSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
    }

    void ListenV4(std::string ip,int port,int backlog)
    {
        struct addrinfo hints, *ai = NULL;
        int err, optval;
        char buf[64];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        sprintf(buf, "%d", port);
        err = getaddrinfo(ip.c_str(), buf, &hints, &ai);
        this->SetREUSEADDR();        
        err = (err!=0)?err:bind(mSocket, ai->ai_addr, ai->ai_addrlen);
        err = (err!=0)?err:listen(mSocket, backlog);

        if(err == 0){
            this->mState = SockState::SS_LISTENING;
        }

        auto it = mListener.find(SEventType::ET_LISTEN);
        if(it != mListener.end())
        {
            SEventArgs args(SEventType::ET_LISTEN,err);
            it->second(this,args);
        } 

        if(err != 0)
        {
            Close();
        }     

        if(ai)freeaddrinfo(ai);
        return;
    }

    int Connect(std::string ip, int port,
                std::function<void(SockHandler *, SEventArgs)> fnCallBack)
    {
        mListener[SEventType::ET_CONNECT] = fnCallBack;
        struct sockaddr_in addr;
        addr.sin_port = port;
        addr.sin_family = AF_INET;
        inet_pton(addr.sin_family, ip.c_str(), &addr.sin_addr);
        int ret = connect(mSocket, (const sockaddr *)&addr, sizeof(addr));
        this->mState = SockState::SS_CONNECTING;
        return ret;
    }

    int Close()
    {
        if (mState == SockState::SS_CLOSED)
            return 1;

        mState = SockState::SS_CLOSED;

        SEventArgs args(SEventType::ET_CLOSE, 0);
        auto it = mListener.find(SEventType::ET_CLOSE);
        if (it != mListener.end())
            it->second(this, args);

        if (mSocket != INVALID_SOCKET)
        {
            close(mSocket);
            mSocket = INVALID_SOCKET;
        }

        //mInBuf.clear();
        mOutBuf.clear();
        return 0;
    }

    SockHandler*Accept()
    {
        SOCKETTYPE sockfd = accept(mSocket, NULL, NULL);
        if(sockfd == INVALID_SOCKET){
            return nullptr;
        }

        SockHandler*handler = new SockHandler(sockfd);
        handler->mState = SockState::SS_CONNECTED;

        //
        auto it = mListener.find(SEventType::ET_ACCETP);
        if(it != mListener.end())
        {
            SEventArgs args(SEventType::ET_ACCETP,0);
            args.accepted_handler = handler;
            mListener[SEventType::ET_ACCETP](this,args);
        }
        return handler;
    }

    void Send(char*buff,size_t sz)
    {
        if(sz == 0)
            return ;
        mOutBuf.insert(mOutBuf.end(),buff,buff+sz);
    }

    void handleEvent(epoll_event e);
    void On(SEventType type, ListenFnType fn) { mListener[type] = fn; }
    void ClearEvents(){ mListener.clear(); }

public:
    void _on_connect_error()
    {
        Close();
        mState = SockState::SS_CLOSED;
        SEventArgs args(SEventType::ET_CONNECT, 1);
        auto it = mListener.find(SEventType::ET_CONNECT);
        if (it != mListener.end())
            it->second(this, args);
    }

    void _on_connected()
    {
        mState = SockState::SS_CONNECTED;
        SEventArgs args(SEventType::ET_CONNECT, 0);
        auto it = mListener.find(SEventType::ET_CONNECT);
        if (it != mListener.end())
            it->second(this, args);
    }

    void _recv() 
    {
        for(;;)
        {
            char data[8192];
            int size = recv(this->mSocket, data, sizeof(data) - 1, 0);
            if(size <= 0)
            {            
                if(size == 0 || last_errno() != EWOULDBLOCK)
                {
                    Close();
                    return;
                }
                else
                {
                    return;
                }
            }

            data[size] = 0;

            auto it = mListener.find(SEventType::ET_DATA);
            if(it != mListener.end())
            {
                SEventArgs args(SEventType::ET_DATA,0);
                args.buff = data;
                args.size = size;
                it->second(this,args);
            }
        }
    }


    int _try_write()
    {
        if (mOutBuf.size())
        {
            int size = send(mSocket, &mOutBuf[0], mOutBuf.size(), 0);
            if (size <= 0)
            {
                if (errno == EWOULDBLOCK)
                    return 0;
                else
                {
                    Close();
                }
            }

            if (size == mOutBuf.size())
                mOutBuf.clear();
            else
            {
                mOutBuf.erase(mOutBuf.begin(), mOutBuf.begin() + size);
            }
        }

        if (mOutBuf.size() == 0)
        {
            if (mState == SockState::SS_CLOSING)
            {
                Close();
                return 0;
            }
        }

        return 1;
    }
};

#define MAX_EVENT_WAIT 10000
class EpollLoop
{
protected:
    EpollLoop()
    {

    }

    std::map<SOCKETTYPE,SockHandler *> mConnects;
    STimerQueue mTimerQueue;
    std::vector<STimer> mQueueToConsume;
    int mStopFlag = 0;
    int mEpFd  = 0;
    std::vector<epoll_event> mEpWaitPool;
public:
    static EpollLoop& GetInstance()
    {
        static EpollLoop ins;
        return ins;
    }

    static uint64_t GetNowTime()
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        //seconds = tv.tv_sec;
        //milliseconds = tv.tv_usec / 1000;
        return tv.tv_sec * 1000 + tv.tv_usec / 1000;
    }
public:
    void Init()
    {
        /* Stops the SIGPIPE signal being raised when writing to a closed socket */
        signal(SIGPIPE, SIG_IGN);
        mEpWaitPool.resize(MAX_EVENT_WAIT);
        mEpFd = epoll_create1(0);
        if(mEpFd <=0) 
        {
            std::cout<<" epoll_create1 error " << std::endl;
            exit(1);
        }
    }

    void UnInit()
    {
    }

    // timer event
    bool AddTimer(int id, int delay, int interval = 0,TimeProcessorFn fn = nullptr)
    {
        if (mTimerQueue.exist(id))
            return false;
        STimer t;
        t.id = id;
        t.delay = delay;
        t.interval = interval;
        t.processer = fn;
        t.trigger_time = EpollLoop::GetNowTime() + delay;
        mTimerQueue.emplace(t);
        return true;
    }
    bool DelTimer(int id)
    {
        return mTimerQueue.remove(id);
    }


    bool IsStop() { return mStopFlag == 1;}
    void SetStop(int flag) { mStopFlag = flag; }
    void Stop()
    {
        SetStop(1);
    }

    void Shutdown()
    {
        while (mTimerQueue.size())
            mTimerQueue.pop();

        for (auto con : mConnects)
        {
            //con->Close();
        }
        //this->Update();
    }

    void pre_process_timer(struct timeval& tv)
    {
        uint64_t now = GetNowTime();
        int64_t waitingInterval = 0;
        if (mTimerQueue.size())
        {
            waitingInterval = mTimerQueue.top().trigger_time - now;
            if (waitingInterval <= 0)
            {
                waitingInterval = 0;
            }
            else
            {
                if (waitingInterval > 1000)
                    waitingInterval = 1000;
            }

            tv.tv_sec = waitingInterval / 1000;
            tv.tv_usec = waitingInterval % 1000 * 1000;
        }
        else
        {
            tv.tv_sec = 0;
            tv.tv_usec = 1000;  // 1 millsec
        }
    }

    void process_timer()
    {
        // handler timer
        uint64_t now = GetNowTime();
        while (mTimerQueue.size())
        {
            if (mTimerQueue.top().trigger_time <= now)
            {
                mQueueToConsume.push_back(mTimerQueue.top());
                mTimerQueue.pop();
            }
            else
            {
                break;
            }
        }

        for (auto& timer : mQueueToConsume)
        {
            if (timer.processer)
                timer.processer(timer.id, timer.delay, timer.interval);

            if (timer.interval > 0)
            {
                bool suc = this->AddTimer(timer.id, timer.interval, timer.interval, timer.processer);
                if (!suc)
                {
                    // 
                    std::cout << "add timer " << timer.id << " error occour" << std::endl;
                }
            }
        }
        mQueueToConsume.clear();
    }
    void Update()
    {
        // handle timer
        struct timeval tv;
        this->pre_process_timer(tv);

        {
            // i/o events hanlde area
            int nfds = epoll_wait(this->mEpFd,&this->mEpWaitPool[0],MAX_EVENT_WAIT,tv.tv_sec*1000+tv.tv_usec/1000);
            for(int i = 0; i< nfds;i++)
            {
                epoll_event&    evt     = this->mEpWaitPool[i];
                int             fd      = evt.data.fd;
                uint32_t        eflags  = evt.events;
                if(mConnects.find(fd) != mConnects.end())
                {
                    mConnects[fd]->handleEvent(evt);
                }
                else
                {
                    // ? todo 
                }
            }
        }

        this->process_timer();
    }

    SockHandler*Listen(std::string ip,int port)
    {
        SockHandler *handler = new SockHandler();
        if(handler->Fd() <= 0)
        {
            delete handler;
            return nullptr;
        }

        //handler->Listen();
        handler->On(SEventType::ET_LISTEN,[](auto*handler,SEventArgs arg){
            if(arg.code == 0)
            {
                std::cout<<" listen start... " << std::endl;
            }
            else
            {
                // error
                std::cout<<" listen error" << std::endl;
            }
        });
        handler->On(SEventType::ET_ACCETP,[](auto*handler,SEventArgs arg)
        {
            std::cout<<" onaccept arg.code = " << arg.code  << " accept-handler: " << arg.accepted_handler << std:: endl;
        });
        mConnects[handler->Fd()] = handler;        
        handler->ListenV4(ip,port,10);
        this->EPOLL_Add(handler,EPOLLIN | EPOLLET);
        return handler;
    }
public:
    int EPOLL_Add(SockHandler*handler,uint32_t eflags)
    {
        epoll_event ee;
        ee.events = eflags;
        ee.data.fd = handler->Fd();
        return epoll_ctl(mEpFd,EPOLL_CTL_ADD,handler->Fd(),&ee);
    }

    int EPOLL_Modify(SockHandler*handler,uint32_t eflags)
    {
        epoll_event ee;
        ee.events = eflags;
        ee.data.fd = handler->Fd();
        return epoll_ctl(mEpFd,EPOLL_CTL_MOD,handler->Fd(),&ee);
    }

    int EPOLL_Del(SockHandler*handler)
    {
        return epoll_ctl(mEpFd,EPOLL_CTL_DEL,handler->Fd(),nullptr);
    }
};


void SockHandler::handleEvent(epoll_event e)
{
    switch(mState)
    {
        case SockState::SS_LISTENING:
        {
            if(e.events & EPOLLIN)
            {
                // new socket
                auto *handler = this->Accept();
                if(handler)
                    EpollLoop::GetInstance().EPOLL_Add(handler,EPOLLIN | EPOLLOUT | EPOLLET);
            }
        }
        break;
        case SockState::SS_CONNECTING:
        {
            if(e.events & (EPOLLIN | EPOLLERR | EPOLLHUP ))
            {
                this->mState = SockState::SS_CONNECTED;
                int optval = 0;
                socklen_t len = sizeof(optval);
                getsockopt(this->mSocket,SOL_SOCKET,SO_ERROR,(char*)&optval,&len);
                if(optval != 0)
                {
                    this->_on_connect_error();                        
                }
                else
                {
                    this->_on_connected();
                }
            }
        }
        break;
        case SockState::SS_CONNECTED:
        {
            if(e.events & (EPOLLIN | EPOLLERR | EPOLLHUP ))
            {
                this->_recv();
            }
            if(e.events & EPOLLOUT)
            {
                this->_try_write();
            }
        }
        break;
    }
    // send data which is getted during from this loop
    this->_try_write();
}

int main()
{
    EpollLoop &loop = EpollLoop::GetInstance();
    loop.Init();
    
    loop.AddTimer(1, 0, 0, [](int id,int delay,int interval) {
        std::cout << "timeid:" << id << " interval:" << interval << " nowtime:" << EpollLoop::GetNowTime() << std::endl;
        });
    loop.AddTimer(2, 10000, 0, [&loop](int id, int delay, int interval) {
        std::cout << "timeid:" << id << " interval:" << interval << " nowtime:" << EpollLoop::GetNowTime() << std::endl;
        //loop.Stop();
        });

    auto*handler = loop.Listen("127.0.0.1",8000);
    handler->SetNoBlock();
    handler->On(SEventType::ET_ACCETP,[](auto*handler,SEventArgs arg)
    {
        std::cout<<" onaccept arg.code = " << arg.code  << " accept-handler: " << arg.accepted_handler << std:: endl;

        auto* clientHandler = arg.accepted_handler;
        clientHandler->On(SEventType::ET_DATA,[](auto*handler, auto arg) {
            std::cout << "recv:" << arg.buff << std::endl;
            handler->Send(arg.buff, arg.size);
        });
        clientHandler->On(SEventType::ET_CLOSE, [](auto* handler, auto arg) {
            std::cout << " socket closed" << std::endl;
        });

    });


    while (!loop.IsStop())
    {
        loop.Update();
    }

    loop.Shutdown();
    loop.UnInit();
    return 0;
}