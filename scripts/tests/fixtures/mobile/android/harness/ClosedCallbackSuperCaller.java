import fixture.*;
public class ClosedCallbackSuperCaller {
    public static Core.Child make(Context context, CharSequence text) {
        return new Core.Child(context,text);
    }
}
