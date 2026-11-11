import fixture.Child;
import fixture.Base;
import fixture.Callback;
import fixture.Adapter;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ConstructorStaticNominalHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned constructor static nominal");}
    private static final class Trap {public String toString(){throw new AssertionError("owned constructor static nominal");}}
    private static Child call(int route,Object a,Object b,Object selector,int index,boolean left,boolean right)throws Throwable {
        if(route==0)return new Child(a,b,selector,index,left,right);if(route==1)return ConstructorStaticNominalCaller.call(a,b,selector,index,left,right);
        if(route==2){try{return Child.class.getConstructor(Object.class,Object.class,Object.class,int.class,boolean.class,boolean.class).newInstance(a,b,selector,index,left,right);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,Object.class,Object.class,Object.class,int.class,boolean.class,boolean.class)).invokeExact(a,b,selector,index,left,right);
    }
    @SuppressWarnings("unchecked")
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Object a=mode==1?null:new Trap(),b=new Trap(),selector=mode==2?null:new Trap();
        final int index=operation==0?Integer.MIN_VALUE+mode:Integer.MAX_VALUE-mode,stepIndex=operation==0?17:-29;
        final boolean left=operation==0,right=mode%2==0;
        final String first=mode==3?null:new String("owned-first"),second=operation==1?first:new String("owned-second");
        final Object desired=mode==8?new Object():mode==9?null:new String("owned-result");
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?5:mode==10?3:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();Trace.factories=0;Trace.events=0;Trace.receiver=null;Trace.adapter=null;Trace.inputLeft=sentinel;Trace.inputRight=sentinel;Trace.payload=sentinel;Trace.left=first;Trace.right=second;Trace.result=sentinel;Trace.callbackFirst=sentinel;Trace.callbackSecond=sentinel;Trace.givenIndex=42;
            Trace.baseAction=new Runnable(){public void run(){Base<?> self=(Base<?>)Trace.receiver;Adapter captured=(Adapter)Trace.adapter;require(Trace.events==1234&&Trace.inputLeft==a&&Trace.inputRight==b&&self.first==first&&self.second==second&&self.callback==captured&&self.left==left&&self.right==right&&captured.seed==a&&captured.selector==selector&&captured.capturedIndex==index&&Trace.payload==sentinel);Trace.payload=payload;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Trace.callbackAction=mode==11?null:new Runnable(){public void run(){require(Trace.events==123456&&Trace.callbackFirst==first&&Trace.callbackSecond==second&&Trace.givenIndex==stepIndex&&Trace.payload==payload);Trace.result=desired;}};
            Child child=null;Object result=sentinel;int length=42;Throwable failure=null;try{child=call(route,a,b,selector,index,left,right);Base<?> self=child;require(self.value==first&&self.callback==Trace.adapter);result=((Callback)self.callback).step(first,second,stepIndex);length=((String)result).length();}catch(Throwable error){failure=error;}
            require(Trace.factories==(mode==6?1:2)&&Trace.events==(mode==6?1:mode==10?123:mode==4||mode==5?1234:mode==7?12345:123456));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7||mode==10?Trace.failure:null;
            if(expected!=null)require(failure==expected&&child==null&&result==sentinel&&length==42);
            else if(mode==8)require(failure instanceof ClassCastException&&child!=null&&result==desired&&length==42);
            else if(mode==9)require(failure instanceof NullPointerException&&child!=null&&result==null&&length==42);
            else if(mode==11)require(failure instanceof NullPointerException&&child!=null&&result==sentinel&&length==42);
            else require(failure==null&&child!=null&&result==desired&&length==((String)desired).length());
            require(Trace.payload==(mode==6||mode==10?sentinel:payload));
            if(mode==6)require(Trace.receiver==null&&Trace.adapter==null&&Trace.inputRight==sentinel);
            else if(mode==10)require(Trace.receiver==null&&Trace.adapter!=null);
            else {Base<?> self=(Base<?>)Trace.receiver;require(self.first==first&&self.second==second&&self.value==(mode==4||mode==5?null:first));}
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
