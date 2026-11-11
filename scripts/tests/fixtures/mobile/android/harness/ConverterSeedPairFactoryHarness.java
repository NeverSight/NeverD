import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ConverterSeedPairFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned converter seed pair factory"); }
    static Result call(int route,Link converter,Object first,Object second,long firstTime,long lastTime,boolean running) throws Throwable {
        try {
            if(route==0)return Client.call(converter,first,second,firstTime,lastTime,running);
            if(route==1)return ConverterSeedPairFactoryCaller.call(converter,first,second,firstTime,lastTime,running);
            if(route==2)return (Result)Provider.class.getMethod("create",Link.class,Object.class,Object.class,long.class,long.class,boolean.class)
                .invoke(null,converter,first,second,firstTime,lastTime,running);
            return (Result)MethodHandles.publicLookup().findStatic(Provider.class,"create",
                MethodType.methodType(Result.class,Link.class,Object.class,Object.class,long.class,long.class,boolean.class))
                .invokeExact(converter,first,second,firstTime,lastTime,running);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method factory=Provider.class.getMethod("create",Link.class,Object.class,Object.class,long.class,long.class,boolean.class);
        TypeVariable<?>[] formals=factory.getTypeParameters();Type[] params=factory.getGenericParameterTypes();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Bound.class);
        require(params[1].equals(formals[0])&&params[2].equals(formals[0]));
        Type[] inputs=((ParameterizedType)params[0]).getActualTypeArguments();
        Type[] outputs=((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments();
        require(inputs[0].equals(formals[0])&&inputs[1].equals(formals[1])&&outputs[0].equals(formals[0])&&outputs[1].equals(formals[1]));
        int[] failures={0,0,0,0,0,1,2,3,4,5,6,0,0,0};
        RuntimeException original=new IllegalArgumentException("owned factory failure");Result previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;
            Object first=mode==1?null:new Object(),second=mode==2?null:new Object();
            Link converter=mode==3?null:new Link<Object,Bound>();
            long firstTime=mode==13?Long.MIN_VALUE:0x8123456789abcdefL+attempt;
            long lastTime=mode==13?Long.MAX_VALUE:0x713579bd02468aceL-attempt;
            boolean running=mode!=4&&attempt==0;
            Result other=new Result<Object,Bound>(new Object(),new Object(),-1,-2,false,converter);
            Trace.events=0;Trace.converter=Trace.extracted=Trace.partial=Trace.created=null;Trace.other=other;
            Trace.kind=mode==11?1:mode==12?2:0;Trace.failAt=failures[mode];Trace.failure=original;
            Result result=null;
            try {result=call(route,converter,first,second,firstTime,lastTime,running);require(Trace.failAt==0);}
            catch(Throwable failure) {require(Trace.failAt!=0&&failure==original);}
            long expected=mode==5?1:mode==6||Trace.kind!=0?12:mode==7?123:mode==8?1234:mode==9?12345:123456;
            require(Trace.events==expected);
            if(mode!=5)require(Trace.converter==converter&&Trace.extracted==first);
            boolean published=Trace.kind==0&&mode!=5&&mode!=6;
            require((Trace.partial!=null)==published);
            if(published) {
                Result partial=(Result)Trace.partial;
                require(partial.first()==(mode==7?null:first));
                require(partial.second()==(mode==7||mode==8?null:second));
                require(partial.converter==(mode==7||mode==8?null:converter));
                require(partial.firstTime==(mode==7||mode==8?0:firstTime));
                require(partial.lastTime==(mode==7||mode==8?0:lastTime));
                require(partial.running==(mode!=7&&mode!=8&&running));
                require(Trace.created==(mode==7||mode==8||mode==9?null:partial));
                if(Trace.failAt==0)require(result==partial&&result!=previous);
            } else require(Trace.created==null);
            if(Trace.kind==1)require(result==other);if(Trace.kind==2)require(result==null);
            previous=result;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
