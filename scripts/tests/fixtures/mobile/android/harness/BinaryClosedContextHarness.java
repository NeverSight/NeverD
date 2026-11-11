import fixture.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
@SuppressWarnings({"rawtypes","unchecked"})
public final class BinaryClosedContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned binary closed context");}
    private static Output call(int route,int operation,Input input,Mapper context)throws Throwable {
        if(route==0)return operation==0?Client.call(input,context):Client.reversed(context,input);
        if(route==1)return operation==0?BinaryClosedContextCaller.call(input,context):BinaryClosedContextCaller.reversed(context,input);
        String name=operation==0?"call":"reversed";
        if(route==2){try{return (Output)(operation==0?Client.class.getMethod(name,Input.class,Mapper.class).invoke(null,input,context):Client.class.getMethod(name,Mapper.class,Input.class).invoke(null,context,input));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Output)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Output.class,Input.class,Mapper.class)).invokeExact(input,context);
        return (Output)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Output.class,Mapper.class,Input.class)).invokeExact(context,input);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Exception checked=new java.io.IOException("owned checked failure");final RuntimeException mapped=new IllegalArgumentException("owned mapped failure"),actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error"),inputError=new Error("owned input error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object(),token=new Object();Trace.events=0;Trace.input=sentinel;Trace.context=sentinel;Trace.result=sentinel;Trace.payload=sentinel;Trace.inputArg=sentinel;Trace.mapArg=sentinel;Trace.outputArg=sentinel;int calls=Trace.calls,ins=Trace.inputCalls,maps=Trace.mapperCalls,outs=Trace.outputCalls;
            final Input input=mode==1?null:new Input(){public Object apply(Object value)throws Exception {require(value==payload);Trace.inputCalls++;Trace.inputArg=value;if(mode==10)throw checked;if(mode==11)throw inputError;return token;}};
            final Mapper context=mode==2?null:new Mapper(){public Object apply(Object value){require(value==checked);Trace.mapperCalls++;Trace.mapArg=value;return mapped;}};
            final Output desiredOutput=new Output(){public Object apply(Object value){require(value==payload&&Trace.payload==payload);Trace.outputCalls++;Trace.outputArg=value;Trace.hit(4);return token;}};
            final Object desired=mode==8?new Object():mode==9?null:desiredOutput;
            final Output sentinelOutput=new Output(){public Object apply(Object value){throw new AssertionError("owned binary closed context");}};
            Trace.action=mode==3?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.input==input&&Trace.context==context&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;try{require(input.apply(payload)==token);}catch(RuntimeException failure){throw failure;}catch(Exception failure){require(failure==checked);throw (RuntimeException)context.apply(failure);}if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            Output actual=sentinelOutput;Object value=sentinel;Throwable failure=null;try{actual=call(route,operation,input,context);value=actual.apply(payload);}catch(Throwable error){failure=error;}
            boolean returned=mode==0||mode==2||mode==9,compared=mode==0||mode==2,inputCalled=mode!=1&&mode!=3&&mode!=6;
            require(Trace.calls==calls+1&&Trace.input==input&&Trace.context==context&&Trace.inputCalls==ins+(inputCalled?1:0)&&Trace.mapperCalls==maps+(mode==10?1:0)&&Trace.outputCalls==outs+(compared?1:0));
            require(Trace.events==(mode==3||mode==6?1:mode==1||mode==4||mode==5||mode==10||mode==11?13:compared?1324:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==10?mapped:mode==11?inputError:null;
            if(expected!=null)require(failure==expected&&value==sentinel);else if(mode==1||mode==3||mode==9)require(failure instanceof NullPointerException&&value==sentinel);else if(mode==8)require(failure instanceof ClassCastException&&value==sentinel);else require(failure==null&&value==token);
            require(actual==(returned?(Output)desired:sentinelOutput)&&Trace.payload==(mode==3||mode==6?sentinel:payload)&&Trace.result==(mode==3||mode==6?sentinel:desired)&&Trace.inputArg==(inputCalled?payload:sentinel)&&Trace.mapArg==(mode==10?checked:sentinel)&&Trace.outputArg==(compared?payload:sentinel));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
