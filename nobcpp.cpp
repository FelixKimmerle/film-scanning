#include "nobcpp.hpp"

int main(int argc, char** argv)
{
    nob::rebuild_self(__FILE__, argc, argv, {"nobcpp.hpp"});

    nob::Build b(argc, argv);
    b.flags.std = 20;
    b.flags.compile_flags.push_back("-pthread");

    auto tiff = b.pkg_config("libtiff-4");

    auto core = b.shared_lib("film_core", "app/core");
    core->link(tiff);

    return b.run();
}
