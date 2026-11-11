package fixture;
public final class Provider {
    public static Context seenA, seenB, seenC;
    public static boolean seenFlag;
    public static Object context, seenItem, sentinel;
    public static Producer pendingProducer;
    public static Completion pendingCompletion;
    public static <T> Object first(Context a, Context b, boolean flag, Context c,
            Producer<? extends T> producer, Completion<? super T> completion) {
        seenA = a; a.touch();
        seenB = b; b.touch();
        seenFlag = flag;
        seenC = c; c.touch();
        context = completion.context();
        pendingProducer = producer; pendingCompletion = completion;
        T item = producer.next(); seenItem = item;
        if (flag) { completion.accept(item); return item; }
        return sentinel;
    }
}
