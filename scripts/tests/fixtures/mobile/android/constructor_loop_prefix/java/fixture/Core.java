package fixture;
public final class Core {
    public final Object value;
    public final long word;
    public Core(Object value,long word){Trace.partial=this;Trace.hit(5);this.value=value;Trace.hit(6);this.word=word;Trace.hit(7);}
    public Core(Iterable<?> input,long word,boolean marker){
        Object last=input;
        while(Trace.next(input)){last=Trace.step(input);}
        Trace.hit(3);
        while(Trace.next(input)){last=Trace.step(input);}
        Trace.hit(4);
        this(last,word);
    }
}
