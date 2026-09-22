import {defineConfig} from '@playwright/test';
export default defineConfig({testDir:'tests',workers:1,timeout:30000,use:{baseURL:'http://127.0.0.1:18088',viewport:{width:1440,height:1000}},reporter:'list'});
