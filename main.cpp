#include "ConsoleInterface.h"
#include "Interface.h"

#include <exception>
#include <iostream>

int main()
{
    Interface interface;
    ConsoleInterface console(interface);
    interface.setConsole(&console);

    // opens storage.db, loads (or creates) our identity and the saved username / port
    try {
        interface.initialize();
    } catch (const std::exception& e) {
        std::cerr << "Could not initialize: " << e.what() << '\n';
        return 1;
    }

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
