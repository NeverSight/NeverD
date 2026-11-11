import fixture.*;
public final class ScopedBuilderCallbackSuperCaller {
    public static Core<String,Object> first(Builder<String,Object> value){return new Core<String,Object>(value);}
    public static Core<String,Object> second(Builder<String,Object> value){return new Core<String,Object>(value,7);}
}
