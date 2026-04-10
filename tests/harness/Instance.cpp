// Instance file parser for the StackSort tuning harness.
// Format:
//   # comments allowed anywhere after the first non-whitespace char
//   grid W H
//   w h type rotatable [name...]
// First non-comment line must be the grid header.

#include "Instance.h"

#include <fstream>
#include <sstream>

static std::string StripComments(const std::string& line)
{
    size_t pos = line.find('#');
    if (pos == std::string::npos) return line;
    return line.substr(0, pos);
}

static std::string Trim(const std::string& s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string StemFromPath(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos) return base;
    return base.substr(0, dot);
}

bool ParseInstanceFile(const std::string& filePath, Instance& out, std::string& errMsg)
{
    std::ifstream f(filePath.c_str());
    if (!f.is_open())
    {
        errMsg = "Cannot open " + filePath;
        return false;
    }

    out.name = StemFromPath(filePath);
    out.gridW = -1;
    out.gridH = -1;
    out.items.clear();

    std::string line;
    int lineNum = 0;
    while (std::getline(f, line))
    {
        ++lineNum;
        std::string cleaned = Trim(StripComments(line));
        if (cleaned.empty()) continue;

        std::istringstream iss(cleaned);

        if (out.gridW < 0)
        {
            std::string tag;
            iss >> tag;
            if (tag != "grid")
            {
                std::ostringstream ee;
                ee << filePath << ":" << lineNum
                   << ": expected 'grid W H' header, got '" << cleaned << "'";
                errMsg = ee.str();
                return false;
            }
            if (!(iss >> out.gridW >> out.gridH) || out.gridW <= 0 || out.gridH <= 0)
            {
                std::ostringstream ee;
                ee << filePath << ":" << lineNum << ": malformed grid header";
                errMsg = ee.str();
                return false;
            }
        }
        else
        {
            Packer::Item item;
            int rotatable = 0;
            if (!(iss >> item.w >> item.h >> item.itemTypeId >> rotatable))
            {
                std::ostringstream ee;
                ee << filePath << ":" << lineNum
                   << ": malformed item line '" << cleaned << "'";
                errMsg = ee.str();
                return false;
            }
            item.id = (int)out.items.size();
            item.canRotate = (rotatable != 0);
            if (item.w <= 0 || item.h <= 0)
            {
                std::ostringstream ee;
                ee << filePath << ":" << lineNum << ": non-positive item dimension";
                errMsg = ee.str();
                return false;
            }
            out.items.push_back(item);
        }
    }

    if (out.gridW < 0)
    {
        errMsg = filePath + ": no grid header found";
        return false;
    }
    if (out.items.empty())
    {
        errMsg = filePath + ": no items";
        return false;
    }

    return true;
}
