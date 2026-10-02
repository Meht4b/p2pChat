#include <ftxui/ftxui.hpp>
#include <iostream>

using namespace ftxui;


void handleInput(const std::string& input)
{
    std::cout << "Received: " << input << '\n';
}


int main()
{
    auto screen = ScreenInteractive::Fullscreen();

    std::string input;

    InputOption input_option;

    input_option.transform = [](InputState state)
    {
        return state.element;
    };

    auto input_component =
        Input(&input, "Enter command...", input_option);


    input_component |= CatchEvent([&](Event event)
    {
        if (event == Event::Return)
        {
            handleInput(input);

            input.clear();

            return true;
        }

        return false;
    });


    auto component = Renderer(input_component, [&]
    {
        return vbox({

            text("P2P CHAT"),

            separator(),

            text("Output") | flex,

            separator(),

            hbox({
                text(">> "),
                input_component->Render()
            })

        });
    });


    screen.Loop(component);
}