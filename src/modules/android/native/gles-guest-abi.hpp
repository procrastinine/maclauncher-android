#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

// Android AAPCS64 gives every spilled scalar an eight-byte slot. Darwin
// packs spilled scalars to their natural alignment. Registers already agree.
// Only signatures whose stack layout differs need a typed, direct adapter.
namespace klj_gles_abi {
template<class F,const char *Name>struct Entry;
template<class R,class... A,const char *Name>struct Entry<R(*)(A...),Name> {
    using Function=R(*)(A...);
    using Arguments=std::tuple<A...>;
    template<size_t I>static consteval bool spilled(){
        constexpr std::array<bool,sizeof...(A)> fp={std::is_floating_point_v<A>...};
        unsigned registers=0;
        for(size_t n=0;n<I;++n)if(fp[n]==fp[I])++registers;
        return registers>=8;
    }
    template<size_t I>using Original=std::tuple_element_t<I,Arguments>;
    template<size_t I>using Guest=std::conditional_t<spilled<I>()&&sizeof(Original<I>)<8&&!std::is_floating_point_v<Original<I>>,uint64_t,Original<I>>;
    template<size_t... I>static consteval bool changed(std::index_sequence<I...>){
        constexpr std::array<bool,sizeof...(A)> stack={spilled<I>()...};
        constexpr std::array<size_t,sizeof...(A)> sizes={sizeof(A)...};
        unsigned android=0,darwin=0;
        for(size_t n=0;n<stack.size();++n)if(stack[n]){
            darwin=(darwin+sizes[n]-1)&~(sizes[n]-1);
            if(darwin!=android)return true;
            android+=8;darwin+=sizes[n];
        }
        return false;
    }
    using Indices=std::index_sequence_for<A...>;
    static constexpr auto indices=Indices{};
    static constexpr bool needs=changed(indices);
    inline static std::atomic<Function> target;
    template<class>struct Call;
    template<size_t... I>struct Call<std::index_sequence<I...>>{
        static_assert(((!spilled<I>()||!std::is_floating_point_v<Original<I>>)&&...),"Spilled GLES floating arguments require a matching register-class adapter");
        static R invoke(Guest<I>... arguments){return target.load(std::memory_order_relaxed)(static_cast<Original<I>>(arguments)...);}
    };
    static void *bind(void *function){
        if constexpr(needs){target.store(reinterpret_cast<Function>(function),std::memory_order_relaxed);return reinterpret_cast<void*>(&Call<Indices>::invoke);}
        return function;
    }
};
}
