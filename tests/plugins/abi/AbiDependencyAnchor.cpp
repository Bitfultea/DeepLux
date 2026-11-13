extern "C" void abi_fixture_dependency_anchor();

namespace {
const int dependencyIsRequired = (abi_fixture_dependency_anchor(), 0);
}
