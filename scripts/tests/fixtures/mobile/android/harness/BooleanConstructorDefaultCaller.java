import fixture.*;
public final class BooleanConstructorDefaultCaller {
    public static Target<String,String> create(int entry,Callback<String,String> mapper) {
        if(entry==0)return new Target<String,String>(mapper);
        if(entry==1)return new Target<String,String>(mapper,(byte)7);
        if(entry==2)return new Target<String,String>(false,mapper);
        if(entry==3)return new Target<String,String>(true,mapper);
        return new Target<String,String>(7,mapper);
    }
    public static String apply(Target<String,String> target,String value) { return target.mapper.apply(value); }
}
