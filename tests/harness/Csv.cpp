// Minimal CSV row writer. Flushes per row so crashes don't lose data.

#include "Csv.h"

CsvWriter::CsvWriter() : fp(NULL) {}

CsvWriter::~CsvWriter()
{
    Close();
}

bool CsvWriter::Open(const std::string& path)
{
    Close();
    fp = fopen(path.c_str(), "w");
    return fp != NULL;
}

void CsvWriter::Close()
{
    if (fp)
    {
        fclose(fp);
        fp = NULL;
    }
}

void CsvWriter::WriteRow(const std::vector<std::string>& values)
{
    if (!fp) return;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0) fputc(',', fp);
        std::string escaped = CsvEscape(values[i]);
        fputs(escaped.c_str(), fp);
    }
    fputc('\n', fp);
    fflush(fp);
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
