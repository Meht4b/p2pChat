#include "ConsoleInterface.h"
#include "Interface.h"

int main()
{
    Interface interface;
    ConsoleInterface console(interface);
    interface.setConsole(&console);
    console.run();
    


   return 0;
}
