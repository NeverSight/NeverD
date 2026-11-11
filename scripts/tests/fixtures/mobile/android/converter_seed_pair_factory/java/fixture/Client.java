package fixture;
public final class Client {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Result call(Link converter,Object first,Object second,long firstTime,long lastTime,boolean running) {
        return Provider.create(converter,first,second,firstTime,lastTime,running);
    }
}
