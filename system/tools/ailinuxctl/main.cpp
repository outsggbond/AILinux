#include <cstdlib>
#include <iostream>
#include <sys/utsname.h>

int main() {
    struct utsname info {};

    std::cout << "AILinux System Control\n";
    std::cout << "Version: 0.1.0\n";

    if (uname(&info) != 0) {
        std::cerr << "Error: cannot read system information\n";
        return EXIT_FAILURE;
    }

    std::cout << "Operating System: " << info.sysname << '\n';
    std::cout << "Kernel Version: " << info.release << '\n';
    std::cout << "Architecture: " << info.machine << '\n';

    return EXIT_SUCCESS;
}
