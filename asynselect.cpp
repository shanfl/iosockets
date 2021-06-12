#ifdef _WIN32
#define _WIN32_WINNT 0x601
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <windows.h>
#include <inttypes.h>
//#define errno WSAGetLastError()
#define close closesocket


#undef  EWOULDBLOCK
#define EWOULDBLOCK WSAEWOULDBLOCK

static int gettimeofday(struct timeval* tp, struct timezone* tzp)
{
    // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
    // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
    // until 00:00:00 January 1, 1970 
    static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    uint64_t    time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((uint64_t)file_time.dwLowDateTime);
    time += ((uint64_t)file_time.dwHighDateTime) << 32;

    tp->tv_sec = (long)((time - EPOCH) / 10000000L);
    tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
    return 0;
}
#endif

#include <functional>
#include <iostream>
#include <map>
#include <vector>
#include <queue>

static int last_errno()
{
#if defined(_WIN32)
    return GetLastError();
#elif defined(__linux__)
    return errno;
#endif
}

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
    SOCKET mSocket = 0;
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

    SockHandler(SOCKET socket) : mSocket(socket)
    {

    }

    void SetNoDelay(int opt)
    {
        opt = !!opt;
        setsockopt(mSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
    }

    void SetNoBlock()
    {
        u_long type = 1;
        ioctlsocket(mSocket, FIONBIO, &type);
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
        SOCKET sockfd = accept(mSocket, NULL, NULL);
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

    void On(SEventType type, ListenFnType fn) { mListener[type] = fn; }
    void ClearEvents(){ mListener.clear(); }

public:
    void _on_connect_error()
    {
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

enum class SelectSetType
{
    SST_READ = 0,
    SST_WRITE,
    SST_EXCEP,
    SST_MAX,
};

class SelectSet
{
public:
    SOCKET maxfd;
    fd_set fds[(int)(SelectSetType::SST_MAX)];

public:
    void zero()
    {
        maxfd = 0;
        for (int i = 0; i < (int)SelectSetType::SST_MAX; i++)
        {
            FD_ZERO(&fds[i]);
        }
    }
    void add(SockHandler *handler, SelectSetType typeSelect)
    {
        if (handler->mSocket > maxfd)
            maxfd = handler->mSocket;
        FD_SET(handler->mSocket, &fds[(int)typeSelect]);
    }

    bool ishas(SockHandler *handler, SelectSetType typeSelect)
    {
        return FD_ISSET(handler->mSocket, &fds[(int)typeSelect]);
    }
};

class SelectLoop
{
public:
    void Init()
    {
#ifdef _WIN32
        WSADATA dat;
        int err = WSAStartup(MAKEWORD(2, 2), &dat);
        if (err != 0)
        {
            std::cout << "WSAStartup failed " << err << std::endl;
        }
#endif
    }

    void UnInit() {}

    void update()
    {

        // clear closed socket
        //
        // handle timer
        struct timeval tv;
        this->pre_process_timer(tv);

        mSelectSet.zero();
        
        for (auto *handler : mConnects)
        {
            switch (handler->mState)
            {
            case SockState::SS_CONNECTING:
            {
                mSelectSet.add(handler, SelectSetType::SST_WRITE);
                mSelectSet.add(handler, SelectSetType::SST_EXCEP);
            }

            break;
            case SockState::SS_CONNECTED:
            {
                mSelectSet.add(handler, SelectSetType::SST_READ);
                if(handler->mOutBuf.size() > 0)
                {
                     mSelectSet.add(handler, SelectSetType::SST_WRITE);
                }
            }
            break;
            case SockState::SS_CLOSING:
            {
                mSelectSet.add(handler, SelectSetType::SST_WRITE);
            }
            break;
            case SockState::SS_LISTENING:
            {
                mSelectSet.add(handler, SelectSetType::SST_READ);
            }
            break;
            case SockState::SS_CLOSED:
            {
            }
            break;
            }
        }

        select(mSelectSet.maxfd + 1, &mSelectSet.fds[0], &mSelectSet.fds[1],
               &mSelectSet.fds[2], &tv);

        for (auto *handler : mConnects)
        {
            switch (handler->mState)
            {
                case SockState::SS_CONNECTING:
                {
                    if (mSelectSet.ishas(handler, SelectSetType::SST_WRITE))
                    {
                        handler->mState = SockState::SS_CONNECTED;
                        int optval = 0;
                        socklen_t optlen = sizeof(optval);
                        getsockopt(handler->mSocket, SOL_SOCKET, SO_ERROR, (char *)&optval,
                                &optlen);
                        if (optval != 0)
                        {
                            // connect failed
                            handler->_on_connect_error();
                        }
                        else
                        {
                            handler->_on_connected();
                        }
                    }
                    else if (mSelectSet.ishas(handler, SelectSetType::SST_EXCEP))
                    {
                        handler->_on_connect_error();
                    }
                }
                break;
                case SockState::SS_CONNECTED:
                {
                    if(mSelectSet.ishas(handler,SelectSetType::SST_READ))
                    {
                        handler->_recv();
                    }

                    if(mSelectSet.ishas(handler,SelectSetType::SST_WRITE))
                    {
                        handler->_try_write();
                    }
                }
                break;
                case SockState::SS_LISTENING:
                {
                    // 
                    if(mSelectSet.ishas(handler,SelectSetType::SST_READ))
                    {
                        this->Accept(handler);
                    }
                }
                break;
            }
            // send data which is getted during from this loop
            handler->_try_write();
        }

        this->process_timer();
    }

    int Connect(std::string ip, int port,
                std::function<void(SockHandler *sh, SEventArgs)> listener)
    {
        SockHandler *handler = new SockHandler();
        mConnects.push_back(handler);
        return handler->Connect(ip, port, listener);
    }

    SockHandler* Accept(SockHandler*handlerListner)
    {
        auto ptr = handlerListner->Accept();
        if(ptr){
            mConnects.push_back(ptr);
        }
        return ptr;
    }

    SockHandler*Listen(std::string ip,int port)
    {
        SockHandler *handler = new SockHandler();
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
        mConnects.push_back(handler);
        handler->ListenV4(ip,port,10);
        return handler;
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

public:
    static uint64_t GetNowTime()
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        //seconds = tv.tv_sec;
        //milliseconds = tv.tv_usec / 1000;
        return tv.tv_sec * 1000 + tv.tv_usec / 1000;
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
        t.trigger_time = SelectLoop::GetNowTime() + delay;
        mTimerQueue.emplace(t);
        return true;
    }
    bool DelTimer(int id)
    {
        return mTimerQueue.remove(id);
    }

private:
    std::vector<SockHandler *> mConnects;
    SelectSet mSelectSet;
    STimerQueue mTimerQueue;
    std::vector<STimer> mQueueToConsume;
};

int main()
{
    SelectLoop loop;
    loop.Init();
    loop.Connect("127.0.0.1", 8080, [](auto *hanlder, SEventArgs args) -> void
                 {
                     std::cout << "connect hander->state:" << (int)hanlder->mState
                               << ",args.code:" << args.code;
                     if (args.code > 0) // Á´½ÓÊ§°Ü
                     {
                     }
                 });
    auto* handler = loop.Listen("0.0.0.0",8000);
    handler->SetNoDelay(1);

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

    loop.AddTimer(1, 0, 0, [](int id,int delay,int interval) {
        std::cout << "timeid:" << id << " interval:" << interval << " nowtime:" << SelectLoop::GetNowTime() << std::endl;
        });
    loop.AddTimer(2, 10000, 0, [](int id, int delay, int interval) {
        std::cout << "timeid:" << id << " interval:" << interval << " nowtime:" << SelectLoop::GetNowTime() << std::endl;
        });
    while (1)
    {
        loop.update();
    }

    return 0;
}