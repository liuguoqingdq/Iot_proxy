#ifndef NO_COPY_ABLE_H
#define NO_COPY_ABLE_H
class nocopyable {
    public:
    nocopyable() =default;
    nocopyable(const nocopyable&) =delete;
    
    nocopyable& operator=(const nocopyable&)=delete;
};
#endif