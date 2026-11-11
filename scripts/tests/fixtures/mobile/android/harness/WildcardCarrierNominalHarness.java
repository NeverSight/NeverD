import fixture.*;

public final class WildcardCarrierNominalHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned wildcard carrier nominal mismatch");
    }
    static Outcome value(String name,Object captured) {
        return new Outcome(name,new Object[]{captured});
    }
    static Object call(int route,Callback callback) {
        switch (route) {
            case 0:return Core.raw(callback);
            case 1:Core.ignore(callback);return null;
            case 2:return Core.relay(callback);
            case 3:return Core.name(callback);
            default:throw new AssertionError();
        }
    }
    public static void main(String[] args) {
        final int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Trace.initializations=0;
        for(int attempt=0;attempt<2;attempt++) {
            final Object input=mode==16?null:new Object(),output=new Object(),poison=new Object();
            final Outcome selected=value("selected",input),late=value("late",poison);
            final RuntimeException runtime=new IllegalArgumentException("owned runtime"),init=new IllegalStateException("owned initializer");
            final Error error=new AssertionError("owned error");
            final int[] callbacks={0},actions={0};
            Trace.calls=Trace.stage=0;Trace.seen=Trace.output=null;Trace.callback=null;
            Trace.input=input;Trace.result=mode==4?null:mode==5?poison:selected;
            Trace.invoke=mode!=1&&mode!=3;Trace.failInitialization=mode==15;Trace.initializationFailure=init;
            final Callback<Object,Object> actual=new Callback<Object,Object>() {
                public Object apply(Object received) {
                    callbacks[0]++;
                    require(Trace.stage==2&&received==input&&Trace.seen==input&&Trace.calls==1);
                    if(mode==7)throw runtime;
                    if(mode==8)throw error;
                    if(mode==11){Trace.input=poison;Trace.result=late;}
                    if(mode==13)Trace.result=poison;
                    return mode==17?null:mode==18?late:mode==19?input:output;
                }
            };
            final Callback callback=mode==2||mode==3?null:actual;
            final Runnable action=new Runnable() {
                public void run() {
                    actions[0]++;
                    require(Trace.stage==3&&Trace.callback==callback&&Trace.calls==1);
                    if(mode==9)throw runtime;
                    if(mode==10)throw error;
                    if(mode==12)Trace.result=late;
                    if(mode==14)Trace.result=null;
                }
            };
            Trace.action=mode==6?null:action;
            Object result=null;Throwable failure=null;
            try{result=call(route,callback);}catch(Throwable caught){failure=caught;}
            if(mode==15) {
                require(Trace.initializations==1&&Trace.calls==0&&Trace.stage==0&&callbacks[0]==0&&actions[0]==0&&Trace.callback==null);
                if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==init);
                else require(failure instanceof NoClassDefFoundError);
                continue;
            }
            require(Trace.initializations==1&&Trace.calls==1&&Trace.callback==callback);
            boolean invoked=mode!=1&&mode!=3;
            int callbackCount=invoked&&callback!=null?1:0;
            require(callbacks[0]==callbackCount&&Trace.seen==(invoked?input:null));
            boolean callbackFailed=mode==2||mode==7||mode==8;
            require(actions[0]==(callbackFailed||mode==6?0:1));
            require(Trace.stage==(callbackFailed?2:mode==6||mode==9||mode==10?3:4));
            Object wantedOutput=mode==17?null:mode==18?late:mode==19?input:output;
            require(Trace.output==(!invoked||callbackFailed?null:wantedOutput));
            require(Trace.input==(mode==11?poison:input));
            if(mode==7||mode==9)require(failure==runtime);
            else if(mode==8||mode==10)require(failure==error);
            else if(mode==2||mode==6||route==3&&(mode==4||mode==14))require(failure instanceof NullPointerException);
            else if(mode==5||mode==13)require(failure instanceof ClassCastException);
            else {
                Outcome expected=mode==4||mode==14?null:mode==11||mode==12?late:selected;
                require(failure==null&&result==(route==1?null:route==3?expected.getImplMethodName():expected));
                if(expected!=null)require(expected.getCapturedArgCount()==1&&expected.getCapturedArg(0)==(expected==late?poison:input));
            }
        }
        System.out.println("attempts=2,wildcard-carrier-nominal=matched");
    }
}
