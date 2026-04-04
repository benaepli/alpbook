export module alpdaq.internal.overloaded;

namespace alpdaq::internal
{
    export template<typename... Ts>
    struct Overloaded : Ts...
    {
        using Ts::operator()...;
    };
    template<class... Ts>
    Overloaded(Ts...) -> Overloaded<Ts...>;
}  // namespace alpdaq::internal