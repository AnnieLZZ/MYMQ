#ifndef TEST_COMMON_HEADER_H
#define TEST_COMMON_HEADER_H
#pragma once // 或者使用传统的 include guards

#include <cstring>
#include<iostream>
#include<error.h>
#include<iostream>
#include<string>
#include"MYMQ_C.h"
#include"MYMQ_PublicCodes.h"
#include <string>
#include <queue>
#include <random>
#include <chrono> // For time-based seeding fallback
#include <stdexcept>
#include<memory>
#include<condition_variable>
#include<mutex>
#include "version.h"

// 仅声明 cerr 函数
void cerr(const std::string& message);
void out(const std::string& str);
std::string generateRandomString(int length, std::mt19937& rng, const std::string& charSet, int randomnessLevel);
double getRepetitionProbability(int randomnessLevel);
std::vector<std::string> generateRandomStringVector(
    int numStrings,
    int minLength,
    int maxLength,
    int randomnessLevel, // 随机性级别参数 (1-5)，越高越随机
    const std::string& charSet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");


#endif // TEST_COMMON_HEADER_H
