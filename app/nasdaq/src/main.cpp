#include <toml++/toml.hpp>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: alpbook_nasdaq <config.toml>\n";
        return 1;
    }

    auto config = toml::parse_file(argv[1]);

    return 0;
}
