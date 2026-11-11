package fixture;
@SuppressWarnings({"rawtypes", "unchecked"})
public class Provider {
    public int calls;
    public String seenContext;
    private final <T> String format(Producer<? extends T> producer, String context) {
        Trace.hit(2); Trace.receiver = this;
        Object value = producer.get();
        Trace.hit(4); calls++; seenContext = context;
        String result = new StringBuilder().append(context).append(value).toString();
        Trace.hit(8); return result;
    }
    public String call(Producer producer, String context) {
        Trace.hit(1);
        String result = format(producer, context);
        Trace.hit(9); return result;
    }
}
