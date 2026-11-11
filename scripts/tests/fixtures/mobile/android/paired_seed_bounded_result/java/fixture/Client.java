package fixture;
public final class Client {
    @SuppressWarnings({"rawtypes","unchecked"})
    public static Result call(Carrier input,Link converter,Object first,Object second,Object third,int kind) {
        return Provider.create(input,converter,first,second,third,kind);
    }
}
