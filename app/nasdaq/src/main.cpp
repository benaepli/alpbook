#include <iostream>

#include <toml++/toml.hpp>

import alpdaq;

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: alpdaq <config.toml>\n";
        return 1;
    }

    toml::table tbl;
    try
    {
        tbl = toml::parse_file(argv[1]);
    }
    catch (toml::parse_error const& e)
    {
        std::cerr << "Config parse error: " << e << "\n";
        return 1;
    }

    auto cfg = alpdaq::config::parse(tbl);
    if (!cfg)
    {
        std::cerr << "Config error: " << cfg.error() << "\n";
        return 1;
    }

    return 0;
}
