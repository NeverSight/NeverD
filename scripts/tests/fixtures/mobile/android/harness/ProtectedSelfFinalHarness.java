import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedSelfFinalHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned protected self final");}
    static Core call(int route,Provider provider,Object input,long word) throws Throwable {
        if(route==0)return new Core(provider,input,word);
        if(route==1)return ProtectedSelfFinalCaller.call(provider,input,word);
        if(route==2)try{return Core.class.getConstructor(Provider.class,Object.class,long.class).newInstance(provider,input,word);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Provider.class,Object.class,long.class)).invokeExact(provider,input,word);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean narrow=Boolean.parseBoolean(args[2]);
        require(Core.class.getFields().length==4&&Core.class.getField("word").getType()==(narrow?int.class:long.class));
        for(Field field:Core.class.getFields())require(Modifier.isFinal(field.getModifiers()));
        Provider provider=new Provider();RuntimeException primary=new IllegalStateException("owned primary"),cleanup=new UnsupportedOperationException("owned cleanup");
        Error observerError=new AssertionError("owned error");Core previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object input=new Object(),first=new Object(),otherFirst=new Object(),lateFirst=new Object(),last=new Object(),otherLast=new Object(),lateLast=new Object(),sentinel=new Object();
            final long word=mode==12?Long.MAX_VALUE:mode==13?Long.MIN_VALUE:mode==14?0x7ff8000000000042L:0x123456789abcdefL+attempt,otherWord=0x7654321089abcdefL-attempt;
            final Object expectedFirst=mode==11?otherFirst:first,expectedLast=mode==10?otherLast:last;
            final long storedWord=narrow?(int)(mode==10?otherWord:word):(mode==10?otherWord:word);
            Trace.events=0;Trace.word=word;Trace.observedWord=0;Trace.partial=null;Trace.firstValue=first;Trace.lastValue=last;Trace.observedInput=sentinel;Trace.failure=primary;
            Trace.failAt=mode>=1&&mode<=8?mode:mode==9?3:0;
            Trace.observer=event->{
                if(event>=2)require(Trace.partial!=null&&Trace.partial.prefix==input);
                if(event==2||event==3)require(Trace.partial.first==null&&Trace.partial.word==0&&Trace.partial.suffix==null);
                if(event==3&&mode==11)Trace.firstValue=otherFirst;
                if(event==4){require(Trace.partial.first==expectedFirst&&Trace.partial.word==0&&Trace.partial.suffix==null);if(mode==10)Trace.word=otherWord;if(mode==11)Trace.firstValue=lateFirst;if(mode==15)throw observerError;}
                if(event==5){boolean firstDone=mode!=3&&mode!=9,wordDone=firstDone&&mode!=4&&mode!=15;
                    require(Trace.partial.first==(firstDone?expectedFirst:null)&&Trace.partial.word==(wordDone?storedWord:0)&&Trace.partial.suffix==null);if(mode==9)throw cleanup;}
                if(event==6||event==7)require(Trace.partial.first==expectedFirst&&Trace.partial.word==storedWord&&Trace.partial.suffix==null);
                if(event==7&&mode==10)Trace.lastValue=otherLast;
                if(event==8){require(Trace.partial.first==expectedFirst&&Trace.partial.word==storedWord&&Trace.partial.suffix==expectedLast);if(mode==11)Trace.lastValue=lateLast;}
            };
            Core result=null;Throwable failure=null;try{result=call(route,provider,input,word);}catch(Throwable e){failure=e;}
            long events=attempt==0?1:0;boolean firstDone=false,wordDone=false,lastDone=false,getter=false;Throwable exact=null;
            if(mode==1){if(attempt==0)require(failure instanceof ExceptionInInitializerError&&failure.getCause()==primary);else require(failure instanceof NoClassDefFoundError);require(Trace.partial==null);}
            else {
                events=events*10+2;
                if(mode==2)exact=primary;
                else {
                    events=events*10+3;getter=true;if(mode==3||mode==9)exact=primary;
                    else{firstDone=true;events=events*10+4;if(mode==4)exact=primary;else if(mode==15)exact=observerError;else wordDone=true;}
                    events=events*10+5;if(mode==5)exact=primary;else if(mode==9)exact=cleanup;
                    if(exact==null){events=events*10+6;if(mode==6)exact=primary;
                        else{events=events*10+7;if(mode==7)exact=primary;else{lastDone=true;events=events*10+8;if(mode==8)exact=primary;}}}
                }
                require(failure==exact&&Trace.partial!=null&&Trace.partial!=previous);
                require(Trace.partial.prefix==input&&Trace.partial.first==(firstDone?expectedFirst:null)&&Trace.partial.word==(wordDone?storedWord:0)&&Trace.partial.suffix==(lastDone?expectedLast:null));
                if(exact==null)require(result==Trace.partial);else require(result==null);
                previous=Trace.partial;
            }
            require(Trace.events==events&&Trace.observedInput==(getter?input:sentinel)&&Trace.observedWord==(getter?word:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
