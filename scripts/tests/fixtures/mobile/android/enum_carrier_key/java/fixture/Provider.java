package fixture;
public class Provider {
    public static <Entry extends Enum<Entry>,Index> Entry select(Pair<Entry,Index> input, Index key) {
        return choose(input,key,null);
    }
    public static <Entry extends Enum<Entry>,Index> Entry choose(Pair<Entry,Index> input, Index key, Entry fallback) {
        Trace.input=input; Trace.key=key; Trace.fallback=fallback; Trace.hit(1);
        Trace.action.run(); Trace.hit(2);
        if(input!=null) Trace.observed=input.probe(key);
        Trace.hit(3);
        return (Entry)Trace.result;
    }
}
