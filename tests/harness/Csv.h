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
    bool Close();
    void WriteRow(const std::vector<std::string>& values);
    bool ok() const
    {
        return fp != NULL && !writeError_;
    }

  private:
    FILE* fp;
    bool writeError_;
};

// Escape a CSV field if it contains commas, quotes, or newlines.
std::string CsvEscape(const std::string& v);
