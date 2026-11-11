import fixture.Client;
public final class ArrayBooleanVarargsCaller {
    public static boolean call(Object[] values){return Client.call(values);}
    public static boolean decide(Object[] values){return Client.decide(values);}
}
