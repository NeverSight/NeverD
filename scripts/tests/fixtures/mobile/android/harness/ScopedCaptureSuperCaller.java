import fixture.*;
public final class ScopedCaptureSuperCaller {
    public static Child<String> defaults(Class<String> type,String[] names) {return new Child<String>(type,names);}
    public static Child<String> direct(boolean flag,Class<String> type,String[] names) {return new Child<String>(flag,type,names);}
}
