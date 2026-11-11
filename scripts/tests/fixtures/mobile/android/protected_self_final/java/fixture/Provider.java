package fixture;
public class Provider {
    public Provider() {}
    public Object first(Object input,long word) {
        Trace.observedInput=input;Trace.observedWord=word;Trace.hit(3);return Trace.firstValue;
    }
    public long second(Object input,long word) {
        Trace.observedInput=input;Trace.observedWord=word;Trace.hit(4);return Trace.word;
    }
    public void restore() { Trace.hit(5); }
    public Object last(Object input,long word) {
        Trace.observedInput=input;Trace.observedWord=word;Trace.hit(7);return Trace.lastValue;
    }
}
