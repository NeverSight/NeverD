package fixture;
public final class Provider {
    public static Object context,seenItem,seenPredicate,miss;
    public static Completion<? super Boolean> booleanCompletion;
    public static <T> Object first(Producer<? extends T> producer,
        Predicate<? super T,? super Completion<? super Boolean>,? extends Object> predicate,
        Completion<? super T> completion) {
        context=completion.context();T item=producer.next();seenItem=item;
        Object test=predicate.apply(item,booleanCompletion);seenPredicate=test;
        if(!((Boolean)test).booleanValue())return miss;
        completion.accept(item);return item;
    }
}
