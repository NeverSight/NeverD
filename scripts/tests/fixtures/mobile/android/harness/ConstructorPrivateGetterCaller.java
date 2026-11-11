import fixture.*;
public final class ConstructorPrivateGetterCaller {
    public static Core create(boolean explicit,int mask,Core fallback,Object token) { return new Core(explicit,mask,fallback,token); }
}
