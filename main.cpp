#include "ConsoleInterface.h"
#include "Interface.h"

#include <exception>
#include <iostream>

int main()
{
    Interface interface;
    ConsoleInterface console(interface);
    interface.setConsole(&console);
    try {
        console.run();
    } catch (const std::exception& e) {
        std::cerr << "Console stopped: " << e.what() << '\n';
        interface.shutdown();
        return 1;
    } catch (...) {
        std::cerr << "Console stopped by an unknown error\n";
        interface.shutdown();
        return 1;
    }
    interface.shutdown();
    return 0;
}
