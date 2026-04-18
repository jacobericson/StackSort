// Minimal CSV row writer. Flushes per row so crashes don't lose data.

#include "Csv.h"

CsvWriter::CsvWriter() : fp(NULL), writeError_(false) {}

CsvWriter::~CsvWriter()
{
    (void)Close();
}

bool CsvWriter::Open(const std::string& path)
{
    (void)Close();
    writeError_ = false;
    fp          = fopen(path.c_str(), "w");
    return fp != NULL;
}

bool CsvWriter::Close()
{
    if (!fp) return !writeError_;
    int rc = fclose(fp);
    fp     = NULL;
    if (rc != 0) writeError_ = true;
    return !writeError_;
}

void CsvWriter::WriteRow(const std::vector<std::string>& values)
{
    if (!fp || writeError_) return;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0)
        {
            if (fputc(',', fp) == EOF)
            {
                writeError_ = true;
                return;
            }
        }
        std::string escaped = CsvEscape(values[i]);
        if (fputs(escaped.c_str(), fp) == EOF)
        {
            writeError_ = true;
            return;
        }
    }
    if (fputc('\n', fp) == EOF)
    {
        writeError_ = true;
        return;
    }
    if (fflush(fp) != 0)
    {
        writeError_ = true;
        return;
    }
}

std::string CsvEscape(const std::string& v)
{
    bool needQuote = false;
    for (size_t i = 0; i < v.size(); ++i)
    {
        char c = v[i];
        if (c == ',' || c == '"' || c == '\n' || c == '\r')
        {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return v;

    std::string out;
    out.reserve(v.size() + 2);
    out += '"';
    for (size_t i = 0; i < v.size(); ++i)
    {
        char c = v[i];
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += '"';
    return out;
}
