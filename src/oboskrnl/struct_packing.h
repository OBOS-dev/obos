#if defined(__GNUC__)
#define OBOS_PACK __attribute__((packed))
#define OBOS_ALIGN(n) __attribute__((aligned(n)))
#elif defined(_MSC_VER)
#define OBOS_PACK __declspec(align(1))
#define OBOS_ALIGN(n) __declspec(align(n))
#endif