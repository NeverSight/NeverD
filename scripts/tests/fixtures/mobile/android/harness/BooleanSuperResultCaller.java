import fixture.*;
public final class BooleanSuperResultCaller {
    public static boolean submit(Core receiver,String value,Object expected,Object update){return receiver.submit(value,expected,update);}
    public static boolean checked(Core receiver,Object value,Object expected,Object update){return receiver.checked(value,expected,update);}
}
