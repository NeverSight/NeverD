import fixture.*;
public final class MixedStarConstructorCaller {
    public static Target create(int entry,Callback<String,String> mapper) {
        if(entry==0)return new Target(mapper);
        if(entry==1)return new Target(mapper,(byte)7);
        if(entry==2)return new Target(false,mapper);
        if(entry==3)return new Target(true,mapper);
        return new Target(7,mapper);
    }
    public static String apply(Target target,String value) { return (String)((Callback)target.mapper).apply(value); }
}
