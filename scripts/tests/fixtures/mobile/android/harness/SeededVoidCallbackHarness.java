import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class SeededVoidCallbackHarness {
    static long events;static int failAt,converts,reads,actions,completions;
    static Object first,second,third,converted,read,result,accepted;
    static RuntimeException failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned seeded void callback");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Convert implements Converter {
        public Bound convert(Object input){converts++;require(input==third);hit(1);return (Bound)converted;}
    }
    static final class Read implements Spec {public Object read(){reads++;hit(2);return read;}}
    static final class Invoke implements Action {
        public Object apply(Object a,Object b){actions++;require(a==first && b==second);hit(3);return result;}
    }
    static final class Finish implements Completion {
        public void accept(Object value){completions++;accepted=value;require(value==result);hit(4);}
    }
    static Object call(int route,Converter converter,Spec spec,Action action,Completion completion) throws Throwable {
        if(route==0)return Client.call(converter,first,second,third,spec,action,completion);
        if(route==1)return SeededVoidCallbackCaller.call(converter,first,second,third,spec,action,completion);
        if(route==2){try{return Provider.class.getMethod("work",Converter.class,Object.class,Object.class,Object.class,Spec.class,Action.class,Completion.class).invoke(null,converter,first,second,third,spec,action,completion);}catch(InvocationTargetException f){throw f.getCause();}}
        return (Object)MethodHandles.publicLookup().findStatic(Provider.class,"work",MethodType.methodType(Object.class,Converter.class,Object.class,Object.class,Object.class,Spec.class,Action.class,Completion.class)).invokeExact(converter,first,second,third,spec,action,completion);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method m=Provider.class.getDeclaredMethod("work",Converter.class,Object.class,Object.class,Object.class,Spec.class,Action.class,Completion.class);
        require(m.getTypeParameters().length==2 && m.getTypeParameters()[0].getBounds()[0]==Object.class && m.getTypeParameters()[1].getBounds()[0]==Bound.class);
        Type[] p=m.getGenericParameterTypes();require(p[1].equals(m.getTypeParameters()[0]) && p[2].equals(p[1]) && p[3].equals(p[1]));
        ParameterizedType cb=(ParameterizedType)p[5],c=(ParameterizedType)p[6];
        require(((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0].equals(p[1]) && ((WildcardType)cb.getActualTypeArguments()[1]).getLowerBounds()[0].equals(p[1]) && cb.getActualTypeArguments()[2]==Unit.class && ((WildcardType)c.getActualTypeArguments()[0]).getLowerBounds()[0]==Unit.class);
        int[] fail={0,0,0,0,0,1,2,3,4,0,0,0,0,0};
        for(int attempt=0;attempt<2;attempt++){
            first=mode==1?null:new Object();second=mode==1?null:new Object();third=mode==1?null:new Object();
            converted=mode==11?null:new Bound(mode==12?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt);read=new Object();
            result=mode==9?null:mode==10?new Object():Unit.VALUE;accepted=null;
            Provider.converted=Provider.read=Provider.callbackResult=null;events=0;converts=reads=actions=completions=0;failAt=fail[mode];failure=new RuntimeException("owned work failure");
            Converter converter=mode==2?null:new Convert();Spec spec=mode==3?null:new Read();Action action=mode==4?null:new Invoke();Completion completion=mode==9?null:new Finish();
            Object actual=null;Throwable caught=null;try{actual=call(route,converter,spec,action,completion);}catch(Throwable f){caught=f;}
            if(mode>=5&&mode<=8)require(caught==failure);else if(mode==2||mode==3||mode==4||mode==9)require(caught instanceof NullPointerException);else if(mode==10)require(caught instanceof ClassCastException);else require(caught==null && actual==result);
            long expected=mode==2?0:mode==3||mode==5?1:mode==4||mode==6?12:mode==7||mode==9||mode==10?123:1234;
            require(events==expected && converts==(mode==2?0:1) && reads==((mode==2||mode==3||mode==5)?0:1) && actions==((mode==2||mode==3||mode==4||mode==5||mode==6)?0:1) && completions==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==7||mode==9||mode==10)?0:1));
            require(Provider.converted==((mode==2||mode==5)?null:converted) && Provider.read==((mode==2||mode==3||mode==5||mode==6)?null:read));
            require(Provider.callbackResult==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==7||mode==10)?null:result));
            if(completions!=0)require(accepted==result);
            if(converted!=null)require(((Bound)converted).bits==(mode==12?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
