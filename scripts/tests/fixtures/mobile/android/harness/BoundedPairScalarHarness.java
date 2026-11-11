import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class BoundedPairScalarHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded pair scalar"); }
    static final class Trap implements Comparable<Object> {
        public int compareTo(Object other) { throw new AssertionError("owned bounded pair scalar eager compare"); }
        public boolean equals(Object other) { throw new AssertionError("owned bounded pair scalar eager equals"); }
        public int hashCode() { throw new AssertionError("owned bounded pair scalar eager hash"); }
        public String toString() { throw new AssertionError("owned bounded pair scalar eager string"); }
    }
    static int call(int route,Comparable left,Comparable right,boolean flag) throws Throwable {
        if(route==0)return Client.call(left,right,flag);
        if(route==1)return BoundedPairScalarCaller.call(left,right,flag);
        if(route==2) {
            try { return (Integer)Client.class.getMethod("call",Comparable.class,Comparable.class,boolean.class).invoke(null,left,right,flag); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        return (int)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(int.class,Comparable.class,Comparable.class,boolean.class)).invokeExact(left,right,flag);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Comparable left=mode==5||mode==11?null:new Trap();
        final Comparable right=mode==6||mode==11?null:mode==7?left:new Trap();
        final boolean flag=kind==1;
        final int expected=mode==9?0:mode==10?Integer.MAX_VALUE:Integer.MIN_VALUE+37+mode+(flag?1:0);
        final RuntimeException runtimeFailure=new RuntimeException("owned callback failure");
        final Error errorFailure=new Error("owned callback error");
        Trace.failure=new RuntimeException("owned stage failure");
        Trace.failAt=mode==1?1:mode==2?2:mode==8?3:0;
        Trace.action=new Runnable() { public void run() {
            require(Trace.left==left&&Trace.right==right&&Trace.flag==flag&&Trace.events==12);
            Trace.hit(4);Trace.result=expected;
            if(mode==3)throw runtimeFailure;if(mode==4)throw errorFailure;
        }};
        for(int attempt=0;attempt<2;attempt++) {
            Object sentinel=new Object();Trace.events=0;Trace.left=Trace.right=sentinel;Trace.flag=!flag;Trace.result=123;
            Throwable failure=null;int actual=42;
            try { actual=call(route,left,right,flag); } catch(Throwable error) { failure=error; }
            require(Trace.events==(mode==1?1:mode==2?12:mode==3||mode==4?124:1243));
            if(mode==1)require(Trace.left==sentinel&&Trace.right==sentinel&&Trace.flag==!flag);
            else require(Trace.left==left&&Trace.right==right&&Trace.flag==flag);
            Throwable wanted=mode==1||mode==2||mode==8?Trace.failure:mode==3?runtimeFailure:mode==4?errorFailure:null;
            require(failure==wanted);
            if(wanted==null)require(actual==expected);
            else require(actual==42);
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
