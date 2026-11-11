package fixture;
public final class Core {
    static { Trace.hit(1); }
    public final Object prefix,first,suffix;
    public final long word;
    public Core(Provider provider,Object input,long word) {
        Trace.partial=this;this.prefix=input;Trace.hit(2);
        try {
            this.first=provider.first(input,word);
            this.word=provider.second(input,word);
        } finally { provider.restore(); }
        Trace.hit(6);
        this.suffix=provider.last(input,word);Trace.hit(8);
    }
}
