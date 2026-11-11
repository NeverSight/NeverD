import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BidirectionalFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bidirectional factory"); }
    static Carrier call(int route,Callback first,Callback second) throws Throwable {
        try {
            if(route==0)return Client.call(first,second);
            if(route==1)return BidirectionalFactoryCaller.call(first,second);
            if(route==2)return (Carrier)Provider.class.getMethod("create",Callback.class,Callback.class).invoke(null,first,second);
            return (Carrier)MethodHandles.publicLookup().findStatic(Provider.class,"create",
                MethodType.methodType(Carrier.class,Callback.class,Callback.class)).invokeExact(first,second);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method method=Provider.class.getMethod("create",Callback.class,Callback.class);
        TypeVariable<?>[] formals=method.getTypeParameters();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Bound.class);
        Type[] result=((ParameterizedType)method.getGenericReturnType()).getActualTypeArguments();
        require(result.length==2 && result[0].equals(formals[0]) && result[1].equals(formals[1]));
        for(int direction=0;direction<2;direction++) {
            ParameterizedType parameter=(ParameterizedType)method.getGenericParameterTypes()[direction];
            Type[] slots=parameter.getActualTypeArguments();
            require(parameter.getRawType()==Callback.class && slots.length==2);
            require(((WildcardType)slots[0]).getLowerBounds()[0].equals(formals[direction]));
            require(((WildcardType)slots[1]).getUpperBounds()[0].equals(formals[1-direction]));
        }
        int[] input={7},counts={0,0};
        Object argument=mode==15?null:input;
        Bound middle=mode==5?null:new Bound();
        Object returned=mode==14?null:input;
        RuntimeException original=new IllegalStateException("owned callback failure");
        Callback<Object,Bound> forward=value->{require(value==argument);counts[0]++;Trace.hit(7);
            if(mode==3)throw original;input[0]++;return middle;};
        Callback<Bound,Object> reverse=value->{require(value==middle);counts[1]++;Trace.hit(8);
            if(mode==4)throw original;input[0]++;return returned;};
        Callback first=mode==1?null:forward,second=mode==2?null:reverse;
        Carrier existing=new Carrier<Object,Bound>(first,second),alternate=new Carrier<Object,Bound>(second,first);
        Object expectedResult=mode==6?null:mode==7?alternate:existing;
        int traceFailure=mode>=8&&mode<=13?mode-7:0;
        int stop=mode==1?3:mode==2?4:mode==3?7:mode==4?8:traceFailure;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.failAt=traceFailure;Trace.failure=original;Trace.input=argument;
            Trace.forward=Trace.reverse=Trace.intermediate=Trace.returned=null;Trace.result=expectedResult;
            int before=input[0],left=counts[0],right=counts[1];
            Throwable failure=null;Carrier actual=null;
            try { actual=call(route,first,second); } catch(Throwable error) { failure=error; }
            long expected=0;boolean leftCalled=false,rightCalled=false,leftFinished=false,rightFinished=false;
            for(int event:new int[]{1,2,3,7,4,8,5,6}) {
                expected=expected*10+event;
                if(event==7){leftCalled=true;leftFinished=mode!=3;}
                if(event==8){rightCalled=true;rightFinished=mode!=4;}
                if(event==stop)break;
            }
            require(Trace.events==expected && counts[0]==left+(leftCalled?1:0) && counts[1]==right+(rightCalled?1:0));
            require(input[0]==before+(leftFinished?1:0)+(rightFinished?1:0));
            require(Trace.forward==(stop==1?null:first) && Trace.reverse==(stop==1?null:second));
            require(Trace.intermediate==(leftFinished?middle:null) && Trace.returned==(rightFinished?returned:null));
            if(mode==1||mode==2)require(failure instanceof NullPointerException);
            else if(stop!=0)require(failure==original);
            else require(failure==null && actual==expectedResult);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
