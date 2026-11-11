import fixture.*;
public final class TypedVariadicConstructorCaller {
    public static Target<String> defaults(Class<String> type,String[] names) {return new Target<String>(type,names);}
    public static Target<String> direct(boolean flag,Class<String> type,String[] names) {return new Target<String>(flag,type,names);}
}
