import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationRetainedIntHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned retained integer allocation"); }
    static boolean frame(StackTraceElement[] frames,String owner,String method) {
        if(frames==null)return false;for(StackTraceElement f:frames)if(f.getClassName().equals(owner)&&f.getMethodName().equals(method))return true;return false;
    }
    static Outer call(int route,Producer producer) throws Throwable {
        if(route==0)return Core.make(producer);
        if(route==1)return AllocationRetainedIntCaller.call(producer);
        if(route==2)try{return (Outer)Core.class.getMethod("make",Producer.class).invoke(null,producer);}catch(InvocationTargetException e){throw e.getCause();}
        return (Outer)MethodHandles.publicLookup().findStatic(Core.class,"make",MethodType.methodType(Outer.class,Producer.class)).invokeExact(producer);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean derived=Boolean.parseBoolean(args[2]),encoded=Boolean.parseBoolean(args[3]);
        Method wrapper=Producer.class.getDeclaredMethod("length");
        require(wrapper.isBridge()==encoded && !wrapper.isSynthetic() && Modifier.isFinal(wrapper.getModifiers()) && wrapper.getReturnType()==int.class);
        boolean plain=args.length>4&&Boolean.parseBoolean(args[4]);
        if(plain)require(Producer.class.getGenericInterfaces()[0]==CountView.class&&CountView.class.getTypeParameters().length==0);
        else {
            require(Producer.class.getGenericInterfaces()[0] instanceof ParameterizedType);
            ParameterizedType api=(ParameterizedType)Producer.class.getGenericInterfaces()[0];require(api.getRawType()==CountView.class&&api.getActualTypeArguments()[0]==Object.class);
        }
        RuntimeException failure=new IllegalStateException("owned retained getter failure");Trace.failure=failure;
        Producer producer=mode==10?null:derived?new Derived():new Producer();Outer previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            int first=mode==11?Integer.MAX_VALUE:mode==12?0x7fc01234:-17-attempt;
            int next=mode==11?Integer.MIN_VALUE:mode==13?first:23+attempt,extra=-73-attempt;
            Trace.events=0;Trace.calls=0;Trace.value=first;Trace.next=next;Trace.delta=derived?mode==11?17:7:0;
            Object sentinel=new Object();Trace.seen=sentinel;Trace.partial=null;Trace.firstFrames=null;Trace.lastFrames=null;
            Trace.failAt=mode==1?1:mode==2?2:mode==3?3:mode==4?4:mode==5?6:mode==6?7:mode==7?8:mode==8?9:mode==9&&derived?5:0;
            Trace.observer=event->{
                if(event==4 && mode==14 && Trace.calls==1)Trace.value=extra;
                if(event==3 && mode==15 && Trace.calls==2)throw failure;
                if(event==6)require(Trace.partial!=null && Trace.partial.first==0 && Trace.partial.second==0);
            };
            Outer result=null;Throwable caught=null;try{result=call(route,producer);}catch(Throwable e){caught=e;}
            int[] full=derived?new int[]{1,2,5,3,4,5,3,4,6,7,8,9}:new int[]{1,2,3,4,3,4,6,7,8,9};
            long events=0;int reads=0,recorded=0;boolean entered=false,stored=false;
            for(int event:full) {
                if(attempt==1 && event<=2)continue;
                if(attempt==1 && (mode==1||mode==2))break;
                if(mode==10 && event>2)break;
                events=events*10+event;
                if(event==3)reads++;
                if(event==4)recorded++;
                if(event==6)entered=true;if(event==7)stored=true;
                if(event==Trace.failAt || mode==15&&event==3&&reads==2)break;
            }
            require(Trace.events==events && Trace.calls==reads);
            if(mode==1||mode==2)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode==10)require(caught instanceof NullPointerException);
            else if(Trace.failAt!=0 || mode==15)require(caught==failure);
            else {
                require(caught==null&&result!=null&&result!=previous&&result.child==Trace.partial);
                require(result.child.first==first+Trace.delta&&result.child.second==(mode==14?extra:next)+Trace.delta);previous=result;
            }
            require(Trace.seen==(reads==0?sentinel:producer));
            require((Trace.partial!=null)==entered);
            if(entered)require(stored?Trace.partial.first==first+Trace.delta&&Trace.partial.second==(mode==14?extra:next)+Trace.delta:Trace.partial.first==0&&Trace.partial.second==0);
            if(recorded==0)require(Trace.firstFrames==null&&Trace.lastFrames==null);
            else {
                require(frame(Trace.firstFrames,"fixture.Producer","length")&&frame(Trace.lastFrames,"fixture.Producer","length"));
                require(frame(Trace.firstFrames,"fixture.Derived","getCount")==derived);
                require((Trace.firstFrames!=Trace.lastFrames)==(recorded==2));
            }
            int current=recorded==0?first:mode==14&&recorded==1?extra:next;
            require(Trace.value==current);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
