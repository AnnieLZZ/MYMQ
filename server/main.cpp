#include"MYMQ_S.h"
#include"../build-server-Desktop_Qt_5_14_2_GCC_64bit-Release/generated/version.h"//按实际位置修改
#include<iostream>
int main(){
    std::cout<< SERVER_VERSION_STRING<<std::endl;
    MYMQ_S mq;
    std::cin.get();


}
