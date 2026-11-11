package fixture;
public final class Client {
    public static Object call(Producer producer,Predicate predicate,Completion completion) { return Provider.first(producer,predicate,completion); }
}
