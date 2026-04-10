#pragma once

#include <stdio.h>
#include <string>
#include <vector>

class CsvWriter
{
  public:
    CsvWriter();
    ~CsvWriter();

    bool Open(const std::string& path);
    void Close();
    void WriteRow(const std::vector<std::string>& values);

  private:
    FILE* fp;
};

// Escape a CSV field if it contains commas, quotes, or newlines.
std::string CsvEscape(const std::string& v);
