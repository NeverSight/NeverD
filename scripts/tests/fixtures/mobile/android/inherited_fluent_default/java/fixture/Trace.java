package fixture;
public final class Trace {
    public static int calls,virtuals,events,choice,fail;
    public static Object receiver,input,replacement;
    public static RuntimeException failure;
    public static Object choose(Object self,Object other) {
        calls++;receiver=self;events=events*10+1;
        if(fail==1)throw failure;
        input=other;events=events*10+2;
        if(fail==2)throw failure;
        if(choice==1)return self;
        if(choice==2)return replacement;
        if(choice==3)return null;
        return other;
    }
}
