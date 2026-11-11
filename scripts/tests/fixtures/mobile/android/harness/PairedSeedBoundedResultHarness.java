import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class PairedSeedBoundedResultHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned paired seed bounded result"); }
    static Result call(int route,Carrier input,Link converter,Object first,Object second,Object third,int kind) throws Throwable {
        try {
            if(route==0)return Client.call(input,converter,first,second,third,kind);
            if(route==1)return PairedSeedBoundedResultCaller.call(input,converter,first,second,third,kind);
            if(route==2)return (Result)Provider.class.getMethod("create",Carrier.class,Link.class,Object.class,Object.class,Object.class,int.class)
                .invoke(null,input,converter,first,second,third,kind);
            return (Result)MethodHandles.publicLookup().findStatic(Provider.class,"create",
                MethodType.methodType(Result.class,Carrier.class,Link.class,Object.class,Object.class,Object.class,int.class))
                .invokeExact(input,converter,first,second,third,kind);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method factory=Provider.class.getMethod("create",Carrier.class,Link.class,Object.class,Object.class,Object.class,int.class);
        TypeVariable<?>[] formals=factory.getTypeParameters();Type[] params=factory.getGenericParameterTypes();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Bound.class);
        require(params[2].equals(formals[0])&&params[3].equals(formals[0])&&params[4].equals(formals[0]));
        require(((ParameterizedType)params[0]).getActualTypeArguments()[0].equals(formals[0]));
        Type[] inputSlots=((ParameterizedType)params[1]).getActualTypeArguments();
        Type[] outputSlots=((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments();
        require(inputSlots[0].equals(formals[0])&&inputSlots[1].equals(formals[1])&&outputSlots[0].equals(formals[0])&&outputSlots[1].equals(formals[1]));
        int[] failures={0,0,0,0,0,1,2,3,4,5,6,0,0,0};
        RuntimeException original=new IllegalArgumentException("owned paired seed failure");Result previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.failAt=0;
            Object value=new Object(),first=mode==1?null:new Object(),second=mode==2?null:new Object(),third=mode==3?null:new Object();
            Carrier input=mode==4?null:new Carrier<Object>(value);
            Link converter=mode==13?null:new Link<Object,Bound>();
            Result other=new Result<Object,Bound>(new Object(),new Object(),new Object(),converter);
            Trace.events=0;Trace.input=Trace.converter=Trace.extracted=Trace.partial=Trace.created=null;Trace.other=other;
            Trace.failAt=failures[mode];Trace.failure=original;
            int kind=mode==11?1:mode==12?2:0;Result result=null;
            boolean failing=Trace.failAt!=0 || mode==4;
            try {result=call(route,input,converter,first,second,third,kind);require(!failing);}
            catch(Throwable failure) {require(failing&&(mode==4?failure instanceof NullPointerException:failure==original));}
            long expected=mode==4||mode==5?1:mode==6||kind!=0?12:mode==7?123:mode==8?1234:mode==9?12345:123456;
            require(Trace.events==expected);
            if(mode!=5)require(Trace.input==input&&Trace.converter==converter);
            require(Trace.extracted==(mode==4||mode==5?null:value));
            boolean published=kind==0 && mode!=4&&mode!=5&&mode!=6;
            require((Trace.partial!=null)==published);
            if(published) {
                Result partial=(Result)Trace.partial;
                require(partial.first()==(mode==7?null:first));
                require(partial.second()==(mode==7||mode==8?null:second));
                require(partial.third()==(mode==7||mode==8?null:third));
                require(partial.converter==(mode==7||mode==8?null:converter));
                require(Trace.created==(mode==7||mode==8||mode==9?null:partial));
                if(!failing)require(result==partial&&result!=previous);
            } else require(Trace.created==null);
            if(kind==1)require(result==other);if(kind==2)require(result==null);
            previous=result;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
