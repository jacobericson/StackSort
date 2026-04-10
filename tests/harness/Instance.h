#pragma once

#include <string>
#include <vector>

#include "Packer.h"

struct Instance
{
    std::string name;
    int gridW;
    int gridH;
    std::vector<Packer::Item> items;
};

bool ParseInstanceFile(const std::string& filePath, Instance& out, std::string& errMsg);
