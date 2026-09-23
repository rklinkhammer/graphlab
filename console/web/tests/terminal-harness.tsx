import React from 'react';
import {createRoot} from 'react-dom/client';
import {RecordedTerminal} from '../src/terminal';
createRoot(document.getElementById('root')!).render(<RecordedTerminal run={{id:'fixture',state:'ready',resources:[{kind:'container',logical:'a'}]}} csrf="fixture" api={async()=>({items:[{id:'session',node:'a',kind:'docker'}]})}/>);
