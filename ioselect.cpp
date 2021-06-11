#ifdef _WIN32
#define _WIN32_WINNT 0x601
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <WS2tcpip.h>
#include <WinSock2.h>
#include <windows.h>

#endif

#include <iostream>

class CSelect
{
public:
    static void init()
    {
#ifdef _WIN32
        WSADATA dat;
        int err = WSAStartup(MAKEWORD(2, 2), &dat);
        if (err != 0)
        {
            std::cout << "WSAStartup failed " << err << std::endl;
        }
#else

#endif
    }

    static void uninit()
    {
#ifdef _WIN32
        WSACleanup();
#else

#endif
    }

    int connect(std::string ip,int port)
    {
        return 0;
    }

    static int getlasterror(){
        return WSAGetLastError();
    }
};

int main(int argc,char*argv[])
{
    CSelect::init();

    SOCKET s = socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(struct sockaddr_in));
    addr.sin_port = htons(8000);
    addr.sin_family = AF_INET;
    //addr.sin_addr.S_un.S_addr = inet_addr("127.0.0.1");
    inet_pton(addr.sin_family,"127.0.0.1",&addr.sin_addr);
    //bind(s,(const struct sockaddr*)&addr,sizeof(addr));
    //connect(s,)
    bool isConect = false;
    while(1)
    {
        if(!isConect)
        {
            int ret = connect(s,(const sockaddr*)&addr,sizeof(addr));
            if(ret < 0){

                std::cout<<"connect failed : code = " << CSelect::getlasterror() << std::endl;
                Sleep(1000);
                continue;
            }
            isConect = true;
        }           
        
        
        fd_set set_read,set_write,set_exception;
        FD_ZERO(&set_read);
        FD_ZERO(&set_write);
        FD_ZERO(&set_exception);
        FD_SET(s,&set_read);
        FD_SET(s,&set_write);
        FD_SET(s,&set_exception);
        struct timeval val;
        val.tv_sec = 0;
        val.tv_usec = 1000;

        int ret_select = select(s + 1,&set_read,&set_write,&set_exception,&val);
        if(ret_select == -1) 
        {
            // error
            break;
        }
        else if (ret_select == 0)
        {
            // timeout | no event
        }
        else if (ret_select > 0)
        {
            // has event
            if(FD_ISSET(s,&set_exception))
            {
                //closesocket(s);
                isConect = false;

                // todo 
                std::cout<<"exception occour ! errorcode : " << CSelect::getlasterror() << std::endl;
            }
            if(FD_ISSET(s,&set_write))
            {
                // todo 
                // send msg
                std::cout<<"can write msg" << std::endl;
                char* buf = "AABBCCDDEE";
                int n = send(s,buf,strlen(buf),0);
                std::cout<<"send " << n  << " bytes" << std::endl;
                Sleep(10000);
            }
            if (FD_ISSET(s,&set_read))
            {
                std::cout<<"can read msg" << std::endl;
                char buf[2048] = {0};
                int size = recv(s,buf,2048 - 1,0);
                if(size == 0 ) 
                {
                    // todo: handle close
                }
                else if (size > 0)
                {
                    // todo: continue to recv until no more data
                    std::cout<<" recv:" << buf << std::endl;
                }
                else if (size < 0)
                {
                    //  EWOULDBLOCK egain
                }
            }
            else
            {
                std::cout<<"cant read msg========>" << std::endl;
            }

        }
    }
    return 0;
}