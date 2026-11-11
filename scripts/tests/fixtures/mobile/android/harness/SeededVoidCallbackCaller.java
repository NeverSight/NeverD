import fixture.*;
public final class SeededVoidCallbackCaller {
    public static Object call(Converter converter,Object first,Object second,Object third,Spec spec,Action action,Completion completion) {
        return Provider.work(converter,first,second,third,spec,action,completion);
    }
}
