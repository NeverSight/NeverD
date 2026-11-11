import fixture.*;
import java.util.*;
import java.util.function.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class CallbackArrayFactoryHarness {
    static void require(boolean ok) {if(!ok)throw new AssertionError("owned callback array factory");}
    static final class Trap implements Function<Object,Object> {
        public Object apply(Object value) {throw new AssertionError("owned callback array factory eager apply");}
    }
    public static final class Future implements Comparator<Object> {
        final Object tag=new Object(),left,right;final RuntimeException failure;final boolean fail;
        Future(Object left,Object right,RuntimeException failure,boolean fail) {this.left=left;this.right=right;this.failure=failure;this.fail=fail;}
        public int compare(Object left,Object right) {require(this.left==left&&this.right==right);Trace.hit(4);if(fail)throw failure;return Trace.answer;}
    }
    static Comparator call(int route,int kind,Function fn,Object[] keys) throws Throwable {
        boolean flag=kind==2;
        if(route==0)return kind==0?Client.defaults(fn,keys):Client.call(fn,flag,keys);
        if(route==1)return kind==0?CallbackArrayFactoryCaller.defaults(fn,keys):CallbackArrayFactoryCaller.call(fn,flag,keys);
        if(route==2) {
            try {return (Comparator)(kind==0?Client.class.getMethod("defaults",Function.class,Object[].class).invoke(null,fn,keys):Client.class.getMethod("call",Function.class,boolean.class,Object[].class).invoke(null,fn,flag,keys));}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (Comparator)MethodHandles.publicLookup().findStatic(Client.class,"defaults",MethodType.methodType(Comparator.class,Function.class,Object[].class)).invokeExact(fn,keys);
        return (Comparator)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Comparator.class,Function.class,boolean.class,Object[].class)).invokeExact(fn,flag,keys);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Function fn=mode==8?null:new Trap();
        final Object initial=new Object(),changed=new Object(),left=new Object(),right=new Object();
        final Object[] keys=mode==7?null:mode==10?new String[]{"owned key"}:mode==9?new Object[0]:new Object[]{initial};
        final RuntimeException callbackFailure=new RuntimeException("owned callback failure"),comparisonFailure=new RuntimeException("owned comparison failure");
        final Error errorFailure=new Error("owned callback error");
        final Future initialResult=new Future(left,right,null,false),replacement=new Future(left,right,comparisonFailure,mode==9);
        final Object desired=mode==5?null:mode==6?new Object():replacement;
        final boolean flag=kind==2;Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?1:mode==2?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object();Trace.events=0;Trace.fn=Trace.keys=sentinel;Trace.flag=!flag;Trace.result=initialResult;Trace.answer=Integer.MIN_VALUE+37+mode;
            if(keys!=null&&keys.length>0&&mode!=10)keys[0]=mode==11?keys:initial;
            Trace.action=new Runnable() {public void run() {
                require(Trace.fn==fn&&Trace.keys==keys&&Trace.flag==flag&&Trace.events==1);
                Trace.hit(3);Trace.result=desired;
                if(keys!=null&&keys.length>0&&mode!=10)keys[0]=changed;
                if(mode==3)throw callbackFailure;if(mode==4)throw errorFailure;
            }};
            Object actual=sentinel;Throwable failure=null;int answer=42;
            try {actual=call(route,kind,fn,keys);answer=((Comparator)actual).compare(left,right);}
            catch(Throwable error) {failure=error;}
            boolean early=mode==1||mode==2||mode==3||mode==4;
            long events=mode==1?1:mode==3||mode==4?13:!early&&mode!=5&&mode!=6?1324:132;
            require(Trace.events==events&&Trace.fn==(mode==1?sentinel:fn)&&Trace.keys==(mode==1?sentinel:keys)&&Trace.flag==(mode==1?!flag:flag));
            Throwable expected=mode==1||mode==2?Trace.failure:mode==3?callbackFailure:mode==4?errorFailure:mode==9?comparisonFailure:null;
            if(expected!=null)require(failure==expected&&actual==(early?sentinel:desired)&&answer==42);
            else if(mode==5)require(failure instanceof NullPointerException&&actual==null&&answer==42);
            else if(mode==6)require(failure instanceof ClassCastException&&actual==sentinel&&answer==42);
            else require(failure==null&&actual==replacement&&answer==Trace.answer);
            require(Trace.result==(mode==1?initialResult:desired));
            if(keys!=null&&keys.length>0&&mode!=10)require(keys[0]==(mode==1?(mode==11?keys:initial):changed));
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
