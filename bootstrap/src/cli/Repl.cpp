#include "Cli.h"
#include "Pipeline.h"
#include "Parser.h"
#include "Vm.h"
#include "Error.h"

#include <iostream>
#include <string>

// ─── REPL ─────────────────────────────────────────────────────────────────────

void runREPL(bool debug)
{
    printBanner();
    std::cout << Colors::GREEN << "  REPL — type 'exit' to quit\n"
              << Colors::RESET << "\n";
    VM vm;
    int n = 1;
    std::string line;
    while (true)
    {
        std::cout << Colors::CYAN << "quantum[" << n++ << "]> " << Colors::RESET;
        if (!std::getline(std::cin, line))
            break;
        if (line == "exit" || line == "quit")
            break;
        if (line.empty())
            continue;
        try
        {
            vm.run(compileSource(line, "<repl>", debug));
        }
        catch (const ParseError &e)
        {
            std::cerr << Colors::RED << "[ParseError] " << Colors::RESET << e.what() << "\n";
        }
        catch (const QuantumError &e)
        {
            std::cerr << Colors::RED << "[" << e.kind << "] " << Colors::RESET << e.what() << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << Colors::RED << "[Error] " << Colors::RESET << e.what() << "\n";
        }
    }
    std::cout << Colors::YELLOW << "\n  Goodbye! 👋\n"
              << Colors::RESET;
}

