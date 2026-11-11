import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class WideFrameCallbackHarness {
    static long events,time;static long scale;static int failAt;static RuntimeException failure;
    static Object a,b,result,written,accepted;static Bound vector;static Carrier scope,animation;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned wide frame callback");}
    static void hit(int n){events=events*10+n;if(failAt==n)throw failure;}
    static final class Read implements Carrier {final int id;Read(int id){this.id=id;}public Object read(long t,long f){require(t==time && f==scale);hit(id);return id==1?a:b;}}
    static final class Write implements Frame {public void write(Object value,Bound v){require(value==a && v==vector);written=value;hit(3);}}
    static final class Invoke implements Action {public Object apply(Object input){require(input==scope);accepted=input;hit(4);return result;}}
    static void call(int route,Frame frame,Action action)throws Throwable{
        if(route==0){Client.call(scope,time,scale,animation,frame,action);return;}
        if(route==1){WideFrameCallbackCaller.call(scope,time,scale,animation,frame,action);return;}
        if(route==2){try{Provider.class.getMethod("access",Carrier.class,long.class,long.class,Carrier.class,Frame.class,Action.class).invoke(null,scope,time,scale,animation,frame,action);}catch(InvocationTargetException f){throw f.getCause();}return;}
        MethodHandles.publicLookup().findStatic(Provider.class,"access",MethodType.methodType(void.class,Carrier.class,long.class,long.class,Carrier.class,Frame.class,Action.class)).invokeExact(scope,time,scale,animation,frame,action);
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method m=Provider.class.getDeclaredMethod("work",Carrier.class,long.class,long.class,Carrier.class,Frame.class,Action.class);
        require(Modifier.isPrivate(m.getModifiers()) && m.getTypeParameters().length==2 && m.getTypeParameters()[0].getBounds()[0]==Object.class && m.getTypeParameters()[1].getBounds()[0]==Bound.class);
        Type[] p=m.getGenericParameterTypes();require(p[0].equals(p[3]) && p[1]==long.class && p[2]==long.class);
        ParameterizedType cb=(ParameterizedType)p[5];require(((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0].equals(p[0]) && cb.getActualTypeArguments()[1]==Unit.class);
        for(int attempt=0;attempt<2;attempt++){
            time=mode==12?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x1234567887654321L+attempt;scale=mode==14?Long.MIN_VALUE:mode==15?Long.MAX_VALUE:0x7654321012345678L-attempt;
            a=mode==1?null:new Object();b=new Object();vector=mode==11?null:new Bound(time);Provider.vector=vector;
            result=mode==9?null:mode==10?new Object():Unit.VALUE;events=0;failAt=mode>=5&&mode<=8?mode-4:0;failure=new RuntimeException("owned frame failure");written=accepted=null;
            Provider.seenScope=Provider.seenAnimation=Provider.seenFrame=Provider.first=Provider.second=Provider.seenUnit=null;Provider.seenTime=0;Provider.seenScale=0;
            scope=mode==2?null:new Read(2);animation=mode==3?null:new Read(1);Frame frame=mode==4?null:new Write();Action action=mode==9?null:new Invoke();
            Throwable caught=null;try{call(route,frame,action);}catch(Throwable f){caught=f;}
            if(mode>=5&&mode<=8)require(caught==failure);else if(mode==2||mode==3||mode==4||mode==9)require(caught instanceof NullPointerException);else if(mode==10)require(caught instanceof ClassCastException);else require(caught==null);
            long expected=mode==3?0:mode==2||mode==5?1:mode==4||mode==6?12:mode==7||mode==9?123:1234;require(events==expected);
            require(Provider.seenScope==scope && Provider.seenAnimation==animation && Provider.seenFrame==frame && Provider.seenTime==time && Provider.seenScale==scale);
            require(Provider.first==((mode==3||mode==5)?null:a) && Provider.second==((mode==2||mode==3||mode==5||mode==6)?null:b));
            require(Provider.seenUnit==((mode==2||mode==3||mode==4||mode==5||mode==6||mode==7||mode==8||mode==9||mode==10)?null:result));
            require(written==((expected==0||expected==1||expected==12)?null:a));if(expected==1234)require(accepted==scope);if(vector!=null)require(vector.bits==time);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
